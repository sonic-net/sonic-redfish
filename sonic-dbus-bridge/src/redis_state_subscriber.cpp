///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2024 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

#include "redis_state_subscriber.hpp"
#include "logger.hpp"
#include <cstring>

namespace
{

// Helper to connect to Redis using TCP first, then fall back to common Unix socket paths.
// This mirrors the resilient strategy used by RedisAdapter so that the bridge can work
// in containers which only have the Redis Unix socket mounted (e.g., docker-sonic-redfish
// with bridge networking).
redisContext* connectRedisWithFallback(const std::string& host,
                                       int port,
                                       const char* contextName)
{
    struct timeval timeout = {2, 0}; // 2 seconds
    redisContext* ctx = nullptr;
    bool connected = false;

    // Try TCP first
    LOG_DEBUG("[RedisStateSubscriber] Attempting TCP connection (%s) to %s:%d...",
              contextName, host.c_str(), port);
    ctx = redisConnectWithTimeout(host.c_str(), port, timeout);

    if (!ctx)
    {
        LOG_ERROR("[RedisStateSubscriber] TCP (%s): Failed to allocate Redis context",
                  contextName);
    }
    else if (ctx->err)
    {
        LOG_DEBUG("[RedisStateSubscriber] TCP (%s): Connection failed: %s (errno: %d)",
                  contextName, ctx->errstr, ctx->err);
        redisFree(ctx);
        ctx = nullptr;
    }
    else
    {
        LOG_INFO("[RedisStateSubscriber] Connected to Redis via TCP (%s): %s:%d",
                 contextName, host.c_str(), port);
        connected = true;
    }

    // If TCP failed, try common Unix socket locations
    if (!connected)
    {
        const char* unixSockets[] = {
            "/var/run/redis/redis.sock",
            "/run/redis/redis.sock",
            "/var/run/redis.sock",
            nullptr
        };

        for (int i = 0; unixSockets[i] != nullptr && !connected; ++i)
        {
            LOG_DEBUG("[RedisStateSubscriber] Attempting Unix socket (%s) connection to %s...",
                      contextName, unixSockets[i]);

            ctx = redisConnectUnixWithTimeout(unixSockets[i], timeout);

            if (!ctx)
            {
                LOG_ERROR("[RedisStateSubscriber] Unix socket (%s): Failed to allocate Redis context",
                          contextName);
            }
            else if (ctx->err)
            {
                LOG_DEBUG("[RedisStateSubscriber] Unix socket (%s): Connection failed: %s (errno: %d)",
                          contextName, ctx->errstr, ctx->err);
                redisFree(ctx);
                ctx = nullptr;
            }
            else
            {
                LOG_INFO("[RedisStateSubscriber] Connected to Redis via Unix socket (%s): %s",
                         contextName, unixSockets[i]);
                connected = true;
            }
        }
    }

    if (!connected || !ctx)
    {
        LOG_ERROR("[RedisStateSubscriber] All Redis connection attempts (%s) failed",
                  contextName);
        return nullptr;
    }

    return ctx;
}

} // anonymous namespace

namespace sonic::dbus_bridge
{

RedisStateSubscriber::RedisStateSubscriber()
    : subContext_(nullptr), getContext_(nullptr), running_(false)
{
    LOG_INFO( "[RedisStateSubscriber] Constructor called");
}

RedisStateSubscriber::~RedisStateSubscriber()
{
    LOG_INFO( "[RedisStateSubscriber] Destructor called");
    stop();
}

bool RedisStateSubscriber::start(const std::string& host, int port,
                                 KeyspaceCallback callback)
{
    LOG_INFO( "[RedisStateSubscriber] ========================================");
    LOG_INFO( "[RedisStateSubscriber] Starting subscriber");
    LOG_INFO( "[RedisStateSubscriber] Redis: %s:%d", host.c_str(), port);
    
    if (running_)
    {
        LOG_WARNING( "[RedisStateSubscriber] Subscriber already running");
        return false;
    }
    
    callback_ = callback;
    
    // Create subscription context (TCP + Unix socket fallback)
    subContext_ = connectRedisWithFallback(host, port, "subscribe");
    if (!subContext_)
    {
        LOG_ERROR( "[RedisStateSubscriber] Subscription connection failed");
        return false;
    }
    
    LOG_INFO( "[RedisStateSubscriber] Subscription connection established");
    
    // Create GET context (for HGETALL) with the same fallback logic
    getContext_ = connectRedisWithFallback(host, port, "get");
    if (!getContext_)
    {
        LOG_ERROR( "[RedisStateSubscriber] GET connection failed");
        redisFree(subContext_);
        subContext_ = nullptr;
        return false;
    }
    
    LOG_INFO( "[RedisStateSubscriber] GET connection established");
    
    // Select STATE_DB (DB 6) for both contexts
    redisReply* reply = (redisReply*)redisCommand(subContext_, "SELECT 6");
    if (!reply || reply->type == REDIS_REPLY_ERROR)
    {
        LOG_ERROR( "[RedisStateSubscriber] Failed to select STATE_DB on subscription context");
        if (reply) freeReplyObject(reply);
        redisFree(subContext_);
        redisFree(getContext_);
        subContext_ = nullptr;
        getContext_ = nullptr;
        return false;
    }
    freeReplyObject(reply);
    
    reply = (redisReply*)redisCommand(getContext_, "SELECT 6");
    if (!reply || reply->type == REDIS_REPLY_ERROR)
    {
        LOG_ERROR( "[RedisStateSubscriber] Failed to select STATE_DB on GET context");
        if (reply) freeReplyObject(reply);
        redisFree(subContext_);
        redisFree(getContext_);
        subContext_ = nullptr;
        getContext_ = nullptr;
        return false;
    }
    freeReplyObject(reply);
    
    LOG_INFO( "[RedisStateSubscriber] STATE_DB (DB 6) selected on both contexts");
    
    // Subscribe to keyspace notifications for HOST_STATE|switch-host
    LOG_INFO( "[RedisStateSubscriber] Subscribing to __keyspace@6__:HOST_STATE|switch-host");
    reply = (redisReply*)redisCommand(subContext_,
        "SUBSCRIBE __keyspace@6__:HOST_STATE|switch-host");
    
    if (!reply || reply->type == REDIS_REPLY_ERROR)
    {
        LOG_ERROR( "[RedisStateSubscriber] Failed to subscribe to keyspace notifications");
        if (reply) freeReplyObject(reply);
        redisFree(subContext_);
        redisFree(getContext_);
        subContext_ = nullptr;
        getContext_ = nullptr;
        return false;
    }
    freeReplyObject(reply);
    
    LOG_INFO( "[RedisStateSubscriber] Subscribed successfully");
    
    // Start subscriber thread
    running_ = true;
    subscriberThread_ = std::thread(&RedisStateSubscriber::subscriberLoop, this);
    
    LOG_INFO( "[RedisStateSubscriber] Subscriber thread started");
    LOG_INFO( "[RedisStateSubscriber] ========================================");
    
    return true;
}

bool RedisStateSubscriber::startMultiKey(const std::string& host, int port,
                                          const std::vector<std::string>& keys,
                                          KeyspaceCallback callback)
{
    LOG_INFO( "[RedisStateSubscriber] ========================================");
    LOG_INFO( "[RedisStateSubscriber] Starting multi-key subscriber");
    LOG_INFO( "[RedisStateSubscriber] Host: %s, Port: %d", host.c_str(), port);
    LOG_INFO( "[RedisStateSubscriber] Subscribing to %zu keys", keys.size());

    if (running_)
    {
        LOG_WARNING( "[RedisStateSubscriber] Already running");
        return false;
    }

    if (keys.empty())
    {
        LOG_ERROR( "[RedisStateSubscriber] No keys provided");
        return false;
    }

    callback_ = callback;

    // Create two Redis contexts: one for subscribing, one for getting data.
    // Use TCP + Unix socket fallback so we can connect in bridge-networked containers
    // that only expose Redis via Unix sockets.
    subContext_ = connectRedisWithFallback(host, port, "subscribe");
    if (!subContext_)
    {
        LOG_ERROR( "[RedisStateSubscriber] Failed to connect to Redis (subscribe)");
        return false;
    }

    getContext_ = connectRedisWithFallback(host, port, "get");
    if (!getContext_)
    {
        LOG_ERROR( "[RedisStateSubscriber] Failed to connect to Redis (get)");
        redisFree(subContext_);
        subContext_ = nullptr;
        return false;
    }

    LOG_INFO( "[RedisStateSubscriber] Connected to Redis");

    // Select STATE_DB (DB 6) on both contexts
    redisReply* reply = (redisReply*)redisCommand(subContext_, "SELECT 6");
    if (!reply || reply->type == REDIS_REPLY_ERROR)
    {
        LOG_ERROR( "[RedisStateSubscriber] Failed to select STATE_DB on subscribe context");
        if (reply) freeReplyObject(reply);
        redisFree(subContext_);
        redisFree(getContext_);
        subContext_ = nullptr;
        getContext_ = nullptr;
        return false;
    }
    freeReplyObject(reply);

    reply = (redisReply*)redisCommand(getContext_, "SELECT 6");
    if (!reply || reply->type == REDIS_REPLY_ERROR)
    {
        LOG_ERROR( "[RedisStateSubscriber] Failed to select STATE_DB on get context");
        if (reply) freeReplyObject(reply);
        redisFree(subContext_);
        redisFree(getContext_);
        subContext_ = nullptr;
        getContext_ = nullptr;
        return false;
    }
    freeReplyObject(reply);

    LOG_INFO( "[RedisStateSubscriber] STATE_DB (DB 6) selected on both contexts");

    // Subscribe to keyspace notifications for all keys
    for (const auto& key : keys)
    {
        std::string channel = "__keyspace@6__:" + key;
        LOG_INFO( "[RedisStateSubscriber] Subscribing to %s", channel.c_str());

        reply = (redisReply*)redisCommand(subContext_, "SUBSCRIBE %s", channel.c_str());

        if (!reply || reply->type == REDIS_REPLY_ERROR)
        {
            LOG_ERROR( "[RedisStateSubscriber] Failed to subscribe to %s", channel.c_str());
            if (reply) freeReplyObject(reply);
            redisFree(subContext_);
            redisFree(getContext_);
            subContext_ = nullptr;
            getContext_ = nullptr;
            return false;
        }
        freeReplyObject(reply);
    }

    LOG_INFO( "[RedisStateSubscriber] Subscribed to all %zu keys successfully", keys.size());

    // Start subscriber thread
    running_ = true;
    subscriberThread_ = std::thread(&RedisStateSubscriber::subscriberLoop, this);

    LOG_INFO( "[RedisStateSubscriber] Subscriber thread started");
    LOG_INFO( "[RedisStateSubscriber] ========================================");

    return true;
}

void RedisStateSubscriber::stop()
{
    if (!running_)
    {
        return;
    }
    
    LOG_INFO( "[RedisStateSubscriber] Stopping subscriber");
    
    running_ = false;
    
    if (subscriberThread_.joinable())
    {
        subscriberThread_.join();
        LOG_INFO( "[RedisStateSubscriber] Subscriber thread joined");
    }
    
    if (subContext_)
    {
        redisFree(subContext_);
        subContext_ = nullptr;
    }
    
    if (getContext_)
    {
        redisFree(getContext_);
        getContext_ = nullptr;
    }
    
    LOG_INFO( "[RedisStateSubscriber] Subscriber stopped");
}

void RedisStateSubscriber::subscriberLoop()
{
    LOG_INFO( "[RedisStateSubscriber] Subscriber loop started");

    while (running_)
    {
        redisReply* reply;
        if (redisGetReply(subContext_, (void**)&reply) != REDIS_OK)
        {
            LOG_ERROR( "[RedisStateSubscriber] Redis subscriber error: %s", subContext_->errstr);
            break;
        }

        if (!reply)
        {
            LOG_WARNING( "[RedisStateSubscriber] Received null reply");
            continue;
        }

        // Expected format: ["message", channel, message]
        if (reply->type == REDIS_REPLY_ARRAY && reply->elements == 3)
        {
            std::string messageType = reply->element[0]->str;
            std::string channel = reply->element[1]->str;
            std::string message = reply->element[2]->str;

            LOG_DEBUG( "[RedisStateSubscriber] Received: type=%s, channel=%s, message=%s",
                   messageType.c_str(), channel.c_str(), message.c_str());

            if (messageType == "message" && message == "hset")
            {
                LOG_INFO( "[RedisStateSubscriber] HSET detected on %s", channel.c_str());
                handleKeyspaceNotification(channel);
            }
        }

        freeReplyObject(reply);
    }

    LOG_INFO( "[RedisStateSubscriber] Subscriber loop ended");
}

void RedisStateSubscriber::handleKeyspaceNotification(const std::string& channel)
{
    LOG_INFO( "[RedisStateSubscriber] ========================================");
    LOG_INFO( "[RedisStateSubscriber] Handling keyspace notification");
    LOG_INFO( "[RedisStateSubscriber] Channel: %s", channel.c_str());

    // Channel format: __keyspace@<db>__:<TABLE>|<key>
    // The first ':' separates the keyspace prefix from the SONiC key, which
    // itself uses '|' as the table/key separator (no ':'), so find_first_of
    // is the safe split.
    size_t pos = channel.find_first_of(':');
    if (pos == std::string::npos)
    {
        LOG_WARNING( "[RedisStateSubscriber] Invalid channel format: %s", channel.c_str());
        return;
    }

    std::string key = channel.substr(pos + 1);
    LOG_INFO( "[RedisStateSubscriber] Key: %s", key.c_str());

    // Get all fields from the hash
    std::map<std::string, std::string> fields = hgetall(key);

    if (fields.empty())
    {
        LOG_WARNING( "[RedisStateSubscriber] No fields found for key: %s", key.c_str());
        LOG_INFO( "[RedisStateSubscriber] ========================================");
        return;
    }

    LOG_INFO( "[RedisStateSubscriber] Retrieved %zu fields from %s", fields.size(), key.c_str());

    // Invoke callback for each field
    for (const auto& [field, value] : fields)
    {
        LOG_INFO( "[RedisStateSubscriber]   %s = %s", field.c_str(), value.c_str());

        if (callback_)
        {
            callback_(key, field, value);
        }
    }

    LOG_INFO( "[RedisStateSubscriber] ========================================");
}

std::map<std::string, std::string> RedisStateSubscriber::hgetall(const std::string& key)
{
    std::map<std::string, std::string> result;

    LOG_DEBUG( "[RedisStateSubscriber] HGETALL %s", key.c_str());

    redisReply* reply = (redisReply*)redisCommand(getContext_, "HGETALL %s", key.c_str());

    if (!reply)
    {
        LOG_ERROR( "[RedisStateSubscriber] HGETALL failed: connection lost");
        return result;
    }

    if (reply->type != REDIS_REPLY_ARRAY)
    {
        LOG_ERROR( "[RedisStateSubscriber] HGETALL failed: unexpected reply type %d", reply->type);
        freeReplyObject(reply);
        return result;
    }

    // Parse field-value pairs
    for (size_t i = 0; i + 1 < reply->elements; i += 2)
    {
        std::string field = reply->element[i]->str;
        std::string value = reply->element[i + 1]->str;
        result[field] = value;

        LOG_DEBUG( "[RedisStateSubscriber]   HGETALL result: %s = %s", field.c_str(), value.c_str());
    }

    freeReplyObject(reply);
    return result;
}

} // namespace sonic::dbus_bridge

