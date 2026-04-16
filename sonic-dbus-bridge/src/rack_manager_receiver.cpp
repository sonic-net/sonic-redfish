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
    const std::string& redisHost, int redisPort)
    : server_(server),
      redisHost_(redisHost),
      redisPort_(redisPort),
      redisCtx_(nullptr)
{}

RackManagerReceiver::~RackManagerReceiver()
{
    if (redisCtx_)
    {
        redisFree(redisCtx_);
        redisCtx_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Initialise: connect to Redis + register D-Bus methods
// ---------------------------------------------------------------------------
bool RackManagerReceiver::initialize()
{
    if (!connectRedis())
    {
        LOG_WARNING("RackManagerReceiver: Redis not available -- "
                    "data will not be persisted until reconnect");
    }

    iface_ = server_.add_interface(RACK_MANAGER_OBJ_PATH, RACK_MANAGER_IFACE);

    iface_->register_method(
        "SubmitAlert",
        [this](const std::string& jsonStr) -> bool {
            LOG_INFO("RackManagerReceiver: SubmitAlert received (%zu bytes)",
                     jsonStr.size());
            return storeFields(jsonStr, getAlertMappings());
        });

    iface_->register_method(
        "SubmitTelemetry",
        [this](const std::string& jsonStr) -> bool {
            LOG_INFO("RackManagerReceiver: SubmitTelemetry received (%zu bytes)",
                     jsonStr.size());
            return storeFields(jsonStr, getTelemetryMappings());
        });

    iface_->initialize();

    LOG_INFO("RackManagerReceiver: D-Bus interface registered at %s",
             RACK_MANAGER_OBJ_PATH);
    return true;
}

// ---------------------------------------------------------------------------
// Redis connection (STATE_DB = index 6)
// ---------------------------------------------------------------------------
bool RackManagerReceiver::connectRedis()
{
    struct timeval timeout = {2, 0};

    // Try TCP first
    redisCtx_ = redisConnectWithTimeout(redisHost_.c_str(), redisPort_, timeout);
    if (redisCtx_ && redisCtx_->err)
    {
        LOG_WARNING("RackManagerReceiver: TCP connect failed: %s",
                    redisCtx_->errstr);
        redisFree(redisCtx_);
        redisCtx_ = nullptr;
    }

    // Fallback to unix sockets
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

    // SELECT 6  (STATE_DB)
    redisReply* reply = static_cast<redisReply*>(
        redisCommand(redisCtx_, "SELECT 6"));
    if (!reply || reply->type == REDIS_REPLY_ERROR)
    {
        LOG_ERROR("RackManagerReceiver: Failed to SELECT STATE_DB (6)");
        if (reply)
        {
            freeReplyObject(reply);
        }
        redisFree(redisCtx_);
        redisCtx_ = nullptr;
        return false;
    }
    freeReplyObject(reply);

    LOG_INFO("RackManagerReceiver: Connected to Redis STATE_DB");
    return true;
}

// ---------------------------------------------------------------------------
// Walk mapping table, extract values from JSON, write to Redis
// ---------------------------------------------------------------------------
bool RackManagerReceiver::storeFields(
    const std::string& jsonStr,
    const std::vector<FieldMapping>& mappings)
{
    // Parse JSON
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::istringstream iss(jsonStr);
    std::string errors;
    if (!Json::parseFromStream(builder, iss, &root, &errors))
    {
        LOG_ERROR("RackManagerReceiver: JSON parse error: %s", errors.c_str());
        return false;
    }

    // Lazy reconnect if we lost the connection
    if (!redisCtx_)
    {
        connectRedis();
    }

    int stored = 0;
    int skipped = 0;
    for (const auto& m : mappings)
    {
        Json::Value val = resolveJsonPath(root, m.jsonPath);
        if (val.isNull())
        {
            ++skipped;
            continue;
        }

        std::string strVal = valueToString(val, m.type);
        if (strVal.empty())
        {
            ++skipped;
            continue;
        }

        if (hset(m.redisKey, m.redisField, strVal))
        {
            ++stored;
            LOG_DEBUG("  %s -> %s %s = %s",
                      m.jsonPath.c_str(), m.redisKey.c_str(),
                      m.redisField.c_str(), strVal.c_str());
        }
    }

    LOG_INFO("RackManagerReceiver: Stored %d fields (%d skipped / %zu total)",
             stored, skipped, mappings.size());
    return stored > 0;
}

// ---------------------------------------------------------------------------
// Redis HSET helper
// ---------------------------------------------------------------------------
bool RackManagerReceiver::hset(const std::string& key,
                               const std::string& field,
                               const std::string& value)
{
    if (!redisCtx_)
    {
        return false;
    }

    redisReply* reply = static_cast<redisReply*>(
        redisCommand(redisCtx_, "HSET %s %s %s",
                     key.c_str(), field.c_str(), value.c_str()));
    if (!reply)
    {
        LOG_ERROR("RackManagerReceiver: HSET failed (connection lost?)");
        redisFree(redisCtx_);
        redisCtx_ = nullptr;
        return false;
    }

    bool ok = (reply->type != REDIS_REPLY_ERROR);
    if (!ok)
    {
        LOG_ERROR("RackManagerReceiver: HSET %s %s error: %s",
                  key.c_str(), field.c_str(), reply->str);
    }
    freeReplyObject(reply);
    return ok;
}

} // namespace sonic::dbus_bridge
