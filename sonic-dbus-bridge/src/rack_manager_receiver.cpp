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
// Alert extraction helpers (SubmitAlert)
// ---------------------------------------------------------------------------

// Append a measurement alert (value + unit + severity + timestamp [+ rscm]).
static void emitMeasurementAlert(const AlertRule& rule,
                                 const Json::Value& measurement,
                                 const std::string& severity,
                                 const std::string& rscm,
                                 const std::string& timestamp,
                                 RedisWrites& writes)
{
    const std::string key = std::string(ALERT_KEY_PREFIX) + rule.alertName;
    writes.emplace_back(key, "value", numberToString(measurement));
    if (!rule.unit.empty())
    {
        writes.emplace_back(key, "unit", rule.unit);
    }
    writes.emplace_back(key, "severity", severity);
    writes.emplace_back(key, "timestamp", timestamp);
    if (!rscm.empty())
    {
        writes.emplace_back(key, "rscm_position", rscm);
    }
}

// Append a stateful leak alert (leak[=severity] + timestamp [+ rscm]).
static void emitLeakAlert(const AlertRule& rule, const std::string& severity,
                          const std::string& rscm,
                          const std::string& timestamp, RedisWrites& writes)
{
    const std::string key = std::string(ALERT_KEY_PREFIX) + rule.alertName;
    writes.emplace_back(key, "leak", severity);
    writes.emplace_back(key, "timestamp", timestamp);
    if (!rscm.empty())
    {
        writes.emplace_back(key, "rscm_position", rscm);
    }
}

// Recursively walk the alert envelope.  Severity / RscmPosition inherit from
// the nearest enclosing object when absent on a node; severity defaults to
// "Normal" (with a WARN) when never supplied.  A node is treated as a leak
// alert when its own key matches a leak rule; each numeric member is matched
// against the measurement rules and fanned out independently.
static void extractAlerts(const std::string& nodeKey, const Json::Value& node,
                          const std::string& inheritedSeverity,
                          bool severityFound, const std::string& inheritedRscm,
                          const std::string& timestamp, RedisWrites& writes)
{
    if (node.isArray())
    {
        for (const auto& elem : node)
        {
            extractAlerts(nodeKey, elem, inheritedSeverity, severityFound,
                          inheritedRscm, timestamp, writes);
        }
        return;
    }

    if (!node.isObject())
    {
        return;
    }

    // Resolve this object's effective context, inheriting when absent.
    std::string severity = inheritedSeverity;
    bool haveSeverity = severityFound;
    if (node.isMember("Severity") && node["Severity"].isString())
    {
        severity = node["Severity"].asString();
        haveSeverity = true;
    }
    std::string rscm = inheritedRscm;
    if (node.isMember("RscmPosition"))
    {
        std::string r = rscmToString(node["RscmPosition"]);
        if (!r.empty()) { rscm = r; }
    }

    const auto& rules = getAlertRules();

    // Is this object itself a stateful (leak) alert?
    for (const auto& rule : rules)
    {
        if (rule.isMeasurement || !matchesName(nodeKey, rule.fieldPattern))
        {
            continue;
        }
        std::string sev = severity;
        if (!haveSeverity)
        {
            sev = "Normal";
            LOG_WARNING("RackManagerReceiver: alert '%s' missing Severity; "
                        "defaulting to Normal (malformed alert JSON)",
                        nodeKey.c_str());
        }
        emitLeakAlert(rule, sev, rscm, timestamp, writes);
        break;
    }

    // Walk members: numeric leaves -> measurement alerts; objects -> recurse.
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
                if (!rule.isMeasurement ||
                    !matchesName(name, rule.fieldPattern))
                {
                    continue;
                }
                std::string sev = severity;
                if (!haveSeverity)
                {
                    sev = "Normal";
                    LOG_WARNING("RackManagerReceiver: alert '%s' missing "
                                "Severity; defaulting to Normal (malformed "
                                "alert JSON)", name.c_str());
                }
                emitMeasurementAlert(rule, child, sev, rscm, timestamp,
                                     writes);
                break;
            }
        }
        else if (child.isObject() || child.isArray())
        {
            extractAlerts(name, child, severity, haveSeverity, rscm,
                          timestamp, writes);
        }
    }
}

// ---------------------------------------------------------------------------
// Telemetry extraction helper (SubmitTelemetry)
// ---------------------------------------------------------------------------

// Recursively walk the telemetry envelope, building the trailing dotted path
// of every scalar leaf (e.g. "InletTempDeviation.InletTemperature").  Each
// leaf path is matched against getTelemetryRules(); the first matching rule
// flattens the value into the single TELEMETRY_KEY hash under its field name
// (first-match-wins, so rule patterns are kept mutually exclusive).
static void extractTelemetry(const std::string& path, const Json::Value& node,
                             RedisWrites& writes)
{
    if (node.isArray())
    {
        for (const auto& elem : node)
        {
            extractTelemetry(path, elem, writes);
        }
        return;
    }

    if (node.isObject())
    {
        for (const auto& name : node.getMemberNames())
        {
            const std::string childPath =
                path.empty() ? name : path + "." + name;
            extractTelemetry(childPath, node[name], writes);
        }
        return;
    }

    // Scalar leaf: resolve against the telemetry rules.
    for (const auto& rule : getTelemetryRules())
    {
        if (!matchesName(path, rule.pathPattern))
        {
            continue;
        }
        std::string strVal = valueToString(node, rule.type);
        if (!strVal.empty())
        {
            writes.emplace_back(TELEMETRY_KEY, rule.redisField,
                                std::move(strVal));
        }
        break;  // first-match-wins
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
// Field-driven (see getTelemetryRules()): every scalar leaf under the "Alarms"
// envelope is matched by its trailing dotted path and flattened into the
// single TELEMETRY_KEY hash.
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

    constexpr const char* kEnvelope = "Alarms";
    if (!root.isObject() || !root.isMember(kEnvelope))
    {
        LOG_ERROR("RackManagerReceiver: telemetry missing '%s' envelope",
                  kEnvelope);
        return false;
    }

    extractTelemetry(/*path=*/"", root[kEnvelope], out.writes);

    if (out.writes.empty())
    {
        LOG_WARNING("RackManagerReceiver: SubmitTelemetry produced no "
                    "recognised telemetry (no matching leaf paths)");
    }
    return true;
}

// ---------------------------------------------------------------------------
// Parse SubmitAlert JSON and recursively extract canonical alerts.  Field-
// driven (see getAlertRules()), so the same rule resolves an entry regardless
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
        extractAlerts(name, root[name], /*inheritedSeverity=*/"",
                      /*severityFound=*/false, /*inheritedRscm=*/"", timestamp,
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
