///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2026 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

#include "rack_manager_receiver.hpp"
#include "logger.hpp"

#include <json/json.h>

#include <sstream>

namespace sonic::dbus_bridge
{

// ---------------------------------------------------------------------------
// JSON path resolver:  "a.b.c"  ->  root["a"]["b"]["c"]
// ---------------------------------------------------------------------------
static Json::Value resolveJsonPath(const Json::Value& root,
                                   const std::string& path)
{
    Json::Value current = root;
    std::istringstream iss(path);
    std::string token;
    while (std::getline(iss, token, '.'))
    {
        if (!current.isObject() || !current.isMember(token))
        {
            return Json::nullValue;
        }
        current = current[token];
    }
    return current;
}

// ---------------------------------------------------------------------------
// Serialise a Json::Value to a string suitable for Redis storage
// ---------------------------------------------------------------------------
static std::string valueToString(const Json::Value& val, FieldType type)
{
    if (val.isNull())
    {
        return "";
    }

    switch (type)
    {
        case FieldType::String:
            return val.isString() ? val.asString() : val.toStyledString();

        case FieldType::Number:
            if (val.isDouble())
            {
                return std::to_string(val.asDouble());
            }
            if (val.isIntegral())
            {
                return std::to_string(val.asInt64());
            }
            return "";

        case FieldType::Integer:
            return val.isIntegral() ? std::to_string(val.asInt64()) : "";

        case FieldType::Boolean:
            return val.isBool() ? (val.asBool() ? "true" : "false") : "";
    }
    return "";
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
// ---------------------------------------------------------------------------
bool RackManagerReceiver::initialize()
{
    iface_ = server_.add_interface(RACK_MANAGER_OBJ_PATH, RACK_MANAGER_IFACE);

    iface_->register_method(
        "SubmitAlert",
        [this](const std::string& jsonStr) -> bool {
            LOG_INFO("RackManagerReceiver: SubmitAlert received (%zu bytes)",
                     jsonStr.size());
            Job j{&getAlertMappings(), {}};
            if (!buildJob(jsonStr, *j.mappings, j))
            {
                return false;
            }
            std::lock_guard<std::mutex> lk(queueMutex_);
            if (queue_.size() >= kMaxQueueDepth)
            {
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
            LOG_INFO("RackManagerReceiver: SubmitTelemetry received (%zu bytes)",
                     jsonStr.size());
            Job j{&getTelemetryMappings(), {}};
            if (!buildJob(jsonStr, *j.mappings, j))
            {
                return false;
            }
            std::lock_guard<std::mutex> lk(queueMutex_);
            if (queue_.size() >= kMaxQueueDepth)
            {
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
// Parse JSON + walk mapping table on the dispatch thread.  Cheap; the
// I/O is what we keep off this thread.
// ---------------------------------------------------------------------------
bool RackManagerReceiver::buildJob(const std::string& jsonStr,
                                   const std::vector<FieldMapping>& mappings,
                                   Job& out)
{
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::istringstream iss(jsonStr);
    std::string errors;
    if (!Json::parseFromStream(builder, iss, &root, &errors))
    {
        LOG_ERROR("RackManagerReceiver: JSON parse error: %s",
                  errors.c_str());
        return false;
    }

    out.writes.reserve(mappings.size());
    for (const auto& m : mappings)
    {
        Json::Value val = resolveJsonPath(root, m.jsonPath);
        if (val.isNull()) { continue; }
        std::string strVal = valueToString(val, m.type);
        if (strVal.empty()) { continue; }
        out.writes.emplace_back(m.redisKey, m.redisField, std::move(strVal));
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
            LOG_ERROR("RackManagerReceiver: redisGetReply failed: %s",
                      redisCtx_->errstr);
            redisFree(redisCtx_);
            redisCtx_ = nullptr;
            return;
        }
        if (reply && reply->type != REDIS_REPLY_ERROR) { ++stored; }
        if (reply) { freeReplyObject(reply); }
    }

    LOG_INFO("RackManagerReceiver: persisted %d/%zu fields",
             stored, job.writes.size());
}

} // namespace sonic::dbus_bridge
