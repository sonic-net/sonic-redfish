///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2026 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

#include "rack_manager_receiver.hpp"
#include "bridge_utils.hpp"
#include "logger.hpp"

#include <json/json.h>

#include <sstream>
#include <tuple>
#include <vector>

namespace sonic::dbus_bridge
{

// Pre-extracted (key, field, value) triples destined for Redis HSET.
using RedisWrites =
    std::vector<std::tuple<std::string, std::string, std::string>>;

// ---------------------------------------------------------------------------
// Unified sensor extraction (SubmitAlert + SubmitTelemetry)
//
// Both payloads are walked identically: each object node may carry a
// "Severity" (inherited by descendants when absent) and either be a leak
// entry (its own key matches the leak rule) or hold numeric measurement
// leaves.  Matches fan out into per-sensor STATE_DB keys.  Telemetry stores
// the full record (value/unit/severity/timestamp for a measurement,
// leak/timestamp for a leak); an alert stores only severity/timestamp (or
// leak/timestamp), matching the platform DB schema.
// ---------------------------------------------------------------------------

enum class ExtractMode
{
    Telemetry,
    Alert
};

// Resolve the effective severity for a node, defaulting to "Normal" (with a
// WARN) when never supplied along the inheritance chain.
static std::string effectiveSeverity(const std::string& severity,
                                     bool severityFound,
                                     const std::string& nodeKey)
{
    if (severityFound)
    {
        return severity;
    }
    LOG_WARNING("RackManagerReceiver: '%s' missing Severity; defaulting to "
                "Normal (malformed JSON)", nodeKey.c_str());
    return "Normal";
}

// Recursively walk a payload envelope, fanning matches out into per-sensor
// RACK_MANAGER_DATA / RACK_MANAGER_ALERT writes.  Severity inherits from the
// nearest enclosing object when absent.  A node is treated as a leak record
// when its own key matches the leak rule; each numeric member is matched
// against the measurement rules and emitted independently (first-match-wins,
// so rule patterns are kept mutually exclusive).
static void extractSensors(const std::string& nodeKey, const Json::Value& node,
                           const std::string& inheritedSeverity,
                           bool severityFound, const std::string& timestamp,
                           ExtractMode mode, RedisWrites& writes)
{
    if (node.isArray())
    {
        for (const auto& elem : node)
        {
            extractSensors(nodeKey, elem, inheritedSeverity, severityFound,
                           timestamp, mode, writes);
        }
        return;
    }

    if (!node.isObject())
    {
        return;
    }

    // Resolve this object's effective severity, inheriting when absent.
    std::string severity = inheritedSeverity;
    bool haveSeverity = severityFound;
    if (node.isMember("Severity") && node["Severity"].isString())
    {
        severity = node["Severity"].asString();
        haveSeverity = true;
    }

    const std::string prefix =
        (mode == ExtractMode::Telemetry) ? DATA_KEY_PREFIX : ALERT_KEY_PREFIX;
    const auto& rules = getSensorRules();

    // Is this object itself a leak record?
    for (const auto& rule : rules)
    {
        if (rule.kind != SensorKind::Leak ||
            !matchesName(nodeKey, rule.fieldPattern))
        {
            continue;
        }
        const std::string key = prefix + rule.sensorName;
        const std::string sev =
            effectiveSeverity(severity, haveSeverity, nodeKey);
        writes.emplace_back(key, "leak", sev);
        writes.emplace_back(key, "timestamp", timestamp);
        break;
    }

    // Walk members: numeric leaves -> measurement records; objects -> recurse.
    for (const auto& name : node.getMemberNames())
    {
        if (name == "Severity" || name == "RscmPosition")
        {
            continue;
        }
        const Json::Value& child = node[name];

        if (child.isNumeric())
        {
            for (const auto& rule : rules)
            {
                if (rule.kind != SensorKind::Measurement ||
                    !matchesName(name, rule.fieldPattern))
                {
                    continue;
                }
                const std::string key = prefix + rule.sensorName;
                const std::string sev =
                    effectiveSeverity(severity, haveSeverity, name);
                if (mode == ExtractMode::Telemetry)
                {
                    writes.emplace_back(key, rule.valueField,
                                        numberToString(child));
                    writes.emplace_back(key, "unit", rule.unit);
                }
                writes.emplace_back(key, "severity", sev);
                writes.emplace_back(key, "timestamp", timestamp);
                break;  // first-match-wins
            }
        }
        else if (child.isObject() || child.isArray())
        {
            extractSensors(name, child, severity, haveSeverity, timestamp,
                           mode, writes);
        }
    }
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------
RackManagerReceiver::RackManagerReceiver(
    sdbusplus::asio::object_server& server,
    const std::string& redisHost, int redisPort, int redisDbIndex)
    : server_(server),
      redisHost_(redisHost),
      redisPort_(redisPort),
      redisDbIndex_(redisDbIndex),
      redisCtx_(nullptr)
{}

RackManagerReceiver::~RackManagerReceiver()
{
    // Tell the worker to stop and wait for it to drain.
    {
        std::lock_guard<std::mutex> lk(queueMutex_);
        stopping_ = true;
    }
    queueCv_.notify_all();
    if (worker_.joinable())
    {
        worker_.join();
    }
    if (redisCtx_)
    {
        redisFree(redisCtx_);
        redisCtx_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Initialise: register D-Bus methods + start worker thread.
// Redis is connected lazily on the worker so initialize() never blocks
// the caller if the database is down at boot.
// Returns false if D-Bus interface registration or worker thread startup
// fails; the caller (BridgeApp) logs the error and continues without OEM
// support.
// ---------------------------------------------------------------------------
bool RackManagerReceiver::initialize()
{
    try
    {
        iface_ = server_.add_interface(RACK_MANAGER_OBJ_PATH, RACK_MANAGER_IFACE);

        iface_->register_method(
            "SubmitAlert",
            [this](const std::string& jsonStr) -> bool {
                jobsReceived_.fetch_add(1, std::memory_order_relaxed);
                LOG_INFO("RackManagerReceiver: SubmitAlert received (%zu bytes)",
                         jsonStr.size());
                Job j{};
                if (!buildAlertJob(jsonStr, j))
                {
                    return false;
                }
                std::lock_guard<std::mutex> lk(queueMutex_);
                if (queue_.size() >= kMaxQueueDepth)
                {
                    jobsDroppedQueueFull_.fetch_add(1, std::memory_order_relaxed);
                    LOG_WARNING("RackManagerReceiver: queue full (%zu); "
                                "dropping SubmitAlert", queue_.size());
                    return false;
                }
                queue_.emplace_back(std::move(j));
                queueCv_.notify_one();
                return true;
            });

        iface_->register_method(
            "SubmitTelemetry",
            [this](const std::string& jsonStr) -> bool {
                jobsReceived_.fetch_add(1, std::memory_order_relaxed);
                LOG_INFO("RackManagerReceiver: SubmitTelemetry received (%zu bytes)",
                         jsonStr.size());
                Job j{};
                if (!buildTelemetryJob(jsonStr, j))
                {
                    return false;
                }
                std::lock_guard<std::mutex> lk(queueMutex_);
                if (queue_.size() >= kMaxQueueDepth)
                {
                    jobsDroppedQueueFull_.fetch_add(1, std::memory_order_relaxed);
                    LOG_WARNING("RackManagerReceiver: queue full (%zu); "
                                "dropping SubmitTelemetry", queue_.size());
                    return false;
                }
                queue_.emplace_back(std::move(j));
                queueCv_.notify_one();
                return true;
            });

        iface_->initialize();

        worker_ = std::thread(&RackManagerReceiver::workerLoop, this);

        LOG_INFO("RackManagerReceiver: D-Bus interface registered at %s",
                 RACK_MANAGER_OBJ_PATH);
        return true;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("RackManagerReceiver: initialization failed: %s", e.what());
        iface_.reset();
        return false;
    }
}

// ---------------------------------------------------------------------------
// Redis connection (STATE_DB index from config; worker-thread only)
// ---------------------------------------------------------------------------
bool RackManagerReceiver::connectRedis()
{
    struct timeval timeout = {2, 0};

    redisCtx_ = redisConnectWithTimeout(redisHost_.c_str(), redisPort_,
                                        timeout);
    if (redisCtx_ && redisCtx_->err)
    {
        LOG_WARNING("RackManagerReceiver: TCP connect failed: %s",
                    redisCtx_->errstr);
        redisFree(redisCtx_);
        redisCtx_ = nullptr;
    }

    if (!redisCtx_)
    {
        const char* sockets[] = {
            "/var/run/redis/redis.sock",
            "/run/redis/redis.sock",
            "/var/run/redis.sock",
            nullptr};
        for (int i = 0; sockets[i] && !redisCtx_; ++i)
        {
            redisCtx_ = redisConnectUnixWithTimeout(sockets[i], timeout);
            if (redisCtx_ && redisCtx_->err)
            {
                redisFree(redisCtx_);
                redisCtx_ = nullptr;
            }
        }
    }

    if (!redisCtx_)
    {
        LOG_ERROR("RackManagerReceiver: All Redis connection attempts failed");
        return false;
    }

    redisReply* reply = static_cast<redisReply*>(
        redisCommand(redisCtx_, "SELECT %d", redisDbIndex_));
    if (!reply || reply->type == REDIS_REPLY_ERROR)
    {
        LOG_ERROR("RackManagerReceiver: Failed to SELECT STATE_DB (%d)",
                  redisDbIndex_);
        if (reply) { freeReplyObject(reply); }
        redisFree(redisCtx_);
        redisCtx_ = nullptr;
        return false;
    }
    freeReplyObject(reply);

    LOG_INFO("RackManagerReceiver: Connected to Redis STATE_DB (idx=%d)",
             redisDbIndex_);
    return true;
}

// ---------------------------------------------------------------------------
// Parse SubmitTelemetry JSON and recursively extract canonical telemetry.
// Field-driven (see getSensorRules()): every top-level key matching the
// ".*Alarms.*" envelope pattern is walked and its measurement/leak entries are
// fanned out into per-sensor RACK_MANAGER_DATA|<name> records.
// ---------------------------------------------------------------------------
bool RackManagerReceiver::buildTelemetryJob(const std::string& jsonStr,
                                            Job& out)
{
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::istringstream iss(jsonStr);
    std::string errors;
    if (!Json::parseFromStream(builder, iss, &root, &errors))
    {
        LOG_ERROR("RackManagerReceiver: telemetry JSON parse error: %s",
                  errors.c_str());
        return false;
    }

    constexpr const char* kEnvelopePattern = ".*Alarms.*";
    if (!root.isObject())
    {
        LOG_ERROR("RackManagerReceiver: telemetry payload is not a JSON object");
        return false;
    }

    const std::string timestamp = isoUtcNow();
    bool matchedEnvelope = false;
    for (const auto& name : root.getMemberNames())
    {
        if (!matchesName(name, kEnvelopePattern, /*caseInsensitive=*/false))
        {
            continue;
        }
        matchedEnvelope = true;
        extractSensors(name, root[name], /*inheritedSeverity=*/"",
                       /*severityFound=*/false, timestamp,
                       ExtractMode::Telemetry, out.writes);
    }

    if (!matchedEnvelope)
    {
        LOG_ERROR("RackManagerReceiver: telemetry missing a '%s' envelope key",
                  kEnvelopePattern);
        return false;
    }

    if (out.writes.empty())
    {
        LOG_WARNING("RackManagerReceiver: SubmitTelemetry produced no "
                    "recognised telemetry (no matching measurement/leak fields)");
    }
    return true;
}

// ---------------------------------------------------------------------------
// Parse SubmitAlert JSON and recursively extract canonical alerts.  Field-
// driven (see getSensorRules()), so the same rule resolves an entry regardless
// of nesting depth or wrapper key name.  Every top-level key matching the
// fixed (case-sensitive) "redfish.*" envelope pattern is processed as an
// independent envelope; their alerts merge into one Redis write batch.
// ---------------------------------------------------------------------------
bool RackManagerReceiver::buildAlertJob(const std::string& jsonStr, Job& out)
{
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::istringstream iss(jsonStr);
    std::string errors;
    if (!Json::parseFromStream(builder, iss, &root, &errors))
    {
        LOG_ERROR("RackManagerReceiver: alert JSON parse error: %s",
                  errors.c_str());
        return false;
    }

    constexpr const char* kEnvelopePattern = "redfish.*";
    if (!root.isObject())
    {
        LOG_ERROR("RackManagerReceiver: alert payload is not a JSON object");
        return false;
    }

    const std::string timestamp = isoUtcNow();
    bool matchedEnvelope = false;
    for (const auto& name : root.getMemberNames())
    {
        if (!matchesName(name, kEnvelopePattern, /*caseInsensitive=*/false))
        {
            continue;
        }
        matchedEnvelope = true;
        extractSensors(name, root[name], /*inheritedSeverity=*/"",
                       /*severityFound=*/false, timestamp, ExtractMode::Alert,
                       out.writes);
    }

    if (!matchedEnvelope)
    {
        LOG_ERROR("RackManagerReceiver: alert missing a '%s' envelope key",
                  kEnvelopePattern);
        return false;
    }

    if (out.writes.empty())
    {
        LOG_WARNING("RackManagerReceiver: SubmitAlert produced no recognised "
                    "alerts (no matching measurement/leak fields)");
    }
    return true;
}

// ---------------------------------------------------------------------------
// Worker thread: pop jobs, pipeline HSETs to Redis.
// ---------------------------------------------------------------------------
void RackManagerReceiver::workerLoop()
{
    while (true)
    {
        Job job;
        {
            std::unique_lock<std::mutex> lk(queueMutex_);
            queueCv_.wait(lk, [this] {
                return stopping_ || !queue_.empty();
            });
            if (stopping_ && queue_.empty()) { return; }
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        drainOne(job);
    }
}

void RackManagerReceiver::drainOne(const Job& job)
{
    if (!redisCtx_ && !connectRedis())
    {
        jobsDroppedNoRedis_.fetch_add(1, std::memory_order_relaxed);
        LOG_WARNING("RackManagerReceiver: dropping %zu writes (no Redis)",
                    job.writes.size());
        return;
    }

    // Pipeline: queue all commands, then read all replies.
    for (const auto& [key, field, value] : job.writes)
    {
        if (redisAppendCommand(redisCtx_, "HSET %s %s %s",
                               key.c_str(), field.c_str(),
                               value.c_str()) != REDIS_OK)
        {
            redisCommandFailures_.fetch_add(1, std::memory_order_relaxed);
            LOG_ERROR("RackManagerReceiver: redisAppendCommand failed: %s",
                      redisCtx_->errstr);
            redisFree(redisCtx_);
            redisCtx_ = nullptr;
            return;
        }
    }

    int stored = 0;
    for (std::size_t i = 0; i < job.writes.size(); ++i)
    {
        redisReply* reply = nullptr;
        if (redisGetReply(redisCtx_,
                          reinterpret_cast<void**>(&reply)) != REDIS_OK)
        {
            redisCommandFailures_.fetch_add(1, std::memory_order_relaxed);
            LOG_ERROR("RackManagerReceiver: redisGetReply failed: %s",
                      redisCtx_->errstr);
            redisFree(redisCtx_);
            redisCtx_ = nullptr;
            return;
        }
        if (reply && reply->type != REDIS_REPLY_ERROR) { ++stored; }
        if (reply) { freeReplyObject(reply); }
    }

    jobsPersisted_.fetch_add(1, std::memory_order_relaxed);
    fieldsPersisted_.fetch_add(static_cast<std::uint64_t>(stored),
                               std::memory_order_relaxed);

    LOG_INFO("RackManagerReceiver: persisted %d/%zu fields",
             stored, job.writes.size());

    logSummaryIfDue();
}

// ---------------------------------------------------------------------------
// Counter summary: emit a single INFO line every kSummaryEveryNJobs jobs so
// operators get a steady heartbeat without having to grep every "persisted"
// line.  Cheaper than exposing each counter via D-Bus and good enough for
// the bridge's current low-rate workload.
// ---------------------------------------------------------------------------
void RackManagerReceiver::logSummaryIfDue()
{
    const std::uint64_t persisted =
        jobsPersisted_.load(std::memory_order_relaxed);
    if (persisted == 0 || (persisted % kSummaryEveryNJobs) != 0)
    {
        return;
    }
    LOG_INFO("RackManagerReceiver: summary "
             "received=%lu persisted=%lu fields=%lu "
             "dropped_queue_full=%lu dropped_no_redis=%lu redis_failures=%lu",
             static_cast<unsigned long>(
                 jobsReceived_.load(std::memory_order_relaxed)),
             static_cast<unsigned long>(persisted),
             static_cast<unsigned long>(
                 fieldsPersisted_.load(std::memory_order_relaxed)),
             static_cast<unsigned long>(
                 jobsDroppedQueueFull_.load(std::memory_order_relaxed)),
             static_cast<unsigned long>(
                 jobsDroppedNoRedis_.load(std::memory_order_relaxed)),
             static_cast<unsigned long>(
                 redisCommandFailures_.load(std::memory_order_relaxed)));
}

} // namespace sonic::dbus_bridge
