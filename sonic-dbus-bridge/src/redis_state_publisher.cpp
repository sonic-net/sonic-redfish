///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2024 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

#include "redis_state_publisher.hpp"
#include "logger.hpp"
#include <chrono>
#include <sstream>
#include <iomanip>
#include <cstring>

namespace sonic::dbus_bridge
{

RedisStatePublisher::RedisStatePublisher()
    : stateDbContext_(nullptr), requestCounter_(0)
{
    LOG_INFO( "[RedisStatePublisher] Constructor called");
}

RedisStatePublisher::~RedisStatePublisher()
{
    LOG_INFO( "[RedisStatePublisher] Destructor called");
    if (stateDbContext_)
    {
        LOG_INFO( "[RedisStatePublisher] Closing Redis connection");
        redisFree(stateDbContext_);
        stateDbContext_ = nullptr;
    }
}

bool RedisStatePublisher::connect(const std::string& host, int port)
{
	LOG_INFO( "[RedisStatePublisher] Connecting to Redis at %s:%d", host.c_str(), port);

	struct timeval timeout = {2, 0}; // 2 seconds timeout
	bool connected = false;

	// Try TCP first (same pattern as RedisAdapter)
	stateDbContext_ = redisConnectWithTimeout(host.c_str(), port, timeout);
	if (!stateDbContext_)
	{
	    LOG_ERROR( "[RedisStatePublisher] TCP: Redis connection failed: allocation error");
	}
	else if (stateDbContext_->err)
	{
	    LOG_DEBUG( "[RedisStatePublisher] TCP: connection failed: %s (errno: %d)",
	               stateDbContext_->errstr, stateDbContext_->err);
	    redisFree(stateDbContext_);
	    stateDbContext_ = nullptr;
	}
	else
	{
	    LOG_INFO( "[RedisStatePublisher] Connected to Redis via TCP: %s:%d",
	              host.c_str(), port);
	    connected = true;
	}

	// If TCP failed, fall back to Unix domain sockets under /var/run/redis
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
	        LOG_DEBUG( "[RedisStatePublisher] Unix socket: attempting %s", unixSockets[i]);
	        stateDbContext_ = redisConnectUnixWithTimeout(unixSockets[i], timeout);

	        if (!stateDbContext_)
	        {
	            LOG_ERROR( "[RedisStatePublisher] Unix socket: failed to allocate Redis context");
	        }
	        else if (stateDbContext_->err)
	        {
	            LOG_DEBUG( "[RedisStatePublisher] Unix socket: connection failed: %s (errno: %d)",
	                       stateDbContext_->errstr, stateDbContext_->err);
	            redisFree(stateDbContext_);
	            stateDbContext_ = nullptr;
	        }
	        else
	        {
	            LOG_INFO( "[RedisStatePublisher] Connected to Redis via Unix socket: %s",
	                      unixSockets[i]);
	            connected = true;
	        }
	    }
	}

	if (!connected || !stateDbContext_)
	{
	    LOG_ERROR( "[RedisStatePublisher] All Redis connection attempts failed");
	    return false;
	}

	// Select STATE_DB (DB 6)
	LOG_INFO( "[RedisStatePublisher] Selecting STATE_DB (DB 6)");
	redisReply* reply = (redisReply*)redisCommand(stateDbContext_, "SELECT 6");

	if (!reply)
	{
	    LOG_ERROR( "[RedisStatePublisher] SELECT command failed: connection lost");
	    redisFree(stateDbContext_);
	    stateDbContext_ = nullptr;
	    return false;
	}

	if (reply->type == REDIS_REPLY_ERROR)
	{
	    LOG_ERROR( "[RedisStatePublisher] SELECT command failed: %s", reply->str);
	    freeReplyObject(reply);
	    redisFree(stateDbContext_);
	    stateDbContext_ = nullptr;
	    return false;
	}

	freeReplyObject(reply);
	LOG_INFO( "[RedisStatePublisher] STATE_DB (DB 6) selected successfully");

	return true;
}

std::string RedisStatePublisher::generateRequestId()
{
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    
    std::lock_guard<std::mutex> lock(redisMutex_);
    requestCounter_++;
    
    std::ostringstream oss;
    oss << "req_" << timestamp << "_" << std::setfill('0') << std::setw(6) << requestCounter_;
    
    std::string requestId = oss.str();
    LOG_DEBUG( "[RedisStatePublisher] Generated request ID: %s", requestId.c_str());
    
    return requestId;
}

std::string RedisStatePublisher::publishHostRequest(const std::string& transition)
{
    LOG_INFO( "[RedisStatePublisher] ========================================");
    LOG_INFO( "[RedisStatePublisher] Publishing host transition request");
    LOG_INFO( "[RedisStatePublisher] Transition: %s", transition.c_str());

    std::string requestId = generateRequestId();
    auto timestamp = std::chrono::system_clock::now().time_since_epoch().count();

    std::map<std::string, std::string> fields = {
        {"requested_transition", transition},
        {"request_id", requestId},
        {"timestamp", std::to_string(timestamp)},
        {"status", "pending"}
    };

    LOG_INFO( "[RedisStatePublisher] Request details:");
    LOG_INFO( "[RedisStatePublisher]   - request_id: %s", requestId.c_str());
    LOG_INFO( "[RedisStatePublisher]   - requested_transition: %s", transition.c_str());
    LOG_INFO( "[RedisStatePublisher]   - timestamp: %ld", timestamp);
    LOG_INFO( "[RedisStatePublisher]   - status: pending");

    // Use SONiC table key format: TABLE_NAME|unique_key
    std::string redisKey = "BMC_HOST_REQUEST|" + requestId;

    if (!hmset(redisKey, fields))
    {
        LOG_ERROR( "[RedisStatePublisher] Failed to publish host request to Redis");
        LOG_ERROR( "[RedisStatePublisher] ========================================");
        return "";
    }

    LOG_INFO( "[RedisStatePublisher] Host request published successfully to %s", redisKey.c_str());
    LOG_INFO( "[RedisStatePublisher] ========================================");

    return requestId;
}

bool RedisStatePublisher::updateSwitchHostState(const std::string& deviceState,
                                                const std::string& deviceStatus)
{
    LOG_INFO( "[RedisStatePublisher] ========================================");
    LOG_INFO( "[RedisStatePublisher] Updating SWITCH_HOST_STATE");
    LOG_INFO( "[RedisStatePublisher]   - device_state: %s", deviceState.c_str());
    LOG_INFO( "[RedisStatePublisher]   - device_status: %s", deviceStatus.c_str());
    
    std::map<std::string, std::string> fields = {
        {"device_state", deviceState},
        {"device_status", deviceStatus}
    };
    
    bool result = hmset("SWITCH_HOST_STATE", fields);
    
    if (result)
    {
        LOG_INFO( "[RedisStatePublisher] SWITCH_HOST_STATE updated successfully");
    }
    else
    {
        LOG_ERROR( "[RedisStatePublisher] Failed to update SWITCH_HOST_STATE");
    }
    
    LOG_INFO( "[RedisStatePublisher] ========================================");
    return result;
}

bool RedisStatePublisher::updateRequestStatus(const std::string& requestId,
                                              const std::string& status)
{
    LOG_INFO( "[RedisStatePublisher] Updating request status");
    LOG_INFO( "[RedisStatePublisher]   - request_id: %s", requestId.c_str());
    LOG_INFO( "[RedisStatePublisher]   - status: %s", status.c_str());

    // Use SONiC table key format: TABLE_NAME|unique_key
    std::string redisKey = "BMC_HOST_REQUEST|" + requestId;

    bool result = hset(redisKey, "status", status);

    if (result)
    {
        LOG_INFO( "[RedisStatePublisher] Request status updated successfully for %s", redisKey.c_str());
    }
    else
    {
        LOG_ERROR( "[RedisStatePublisher] Failed to update request status for %s", redisKey.c_str());
    }

    return result;
}

bool RedisStatePublisher::hset(const std::string& key,
                               const std::string& field,
                               const std::string& value)
{
    LOG_DEBUG( "[RedisStatePublisher] HSET %s %s %s", key.c_str(), field.c_str(), value.c_str());

    std::lock_guard<std::mutex> lock(redisMutex_);

    if (!stateDbContext_)
    {
        LOG_ERROR( "[RedisStatePublisher] HSET failed: not connected to Redis");
        return false;
    }

    redisReply* reply = (redisReply*)redisCommand(stateDbContext_,
        "HSET %s %s %s", key.c_str(), field.c_str(), value.c_str());

    if (!reply)
    {
        LOG_ERROR( "[RedisStatePublisher] HSET failed: connection lost (key=%s, field=%s)",
               key.c_str(), field.c_str());
        return false;
    }

    if (reply->type == REDIS_REPLY_ERROR)
    {
        LOG_ERROR( "[RedisStatePublisher] HSET failed: %s (key=%s, field=%s)",
               reply->str, key.c_str(), field.c_str());
        freeReplyObject(reply);
        return false;
    }

    LOG_DEBUG( "[RedisStatePublisher] HSET successful (key=%s, field=%s)", key.c_str(), field.c_str());
    freeReplyObject(reply);
    return true;
}

bool RedisStatePublisher::hmset(const std::string& key,
                                const std::map<std::string, std::string>& fields)
{
    LOG_DEBUG( "[RedisStatePublisher] HMSET %s with %zu fields", key.c_str(), fields.size());

    std::lock_guard<std::mutex> lock(redisMutex_);

    if (!stateDbContext_)
    {
        LOG_ERROR( "[RedisStatePublisher] HMSET failed: not connected to Redis");
        return false;
    }

    // Build HMSET command arguments
    std::vector<const char*> argv;
    std::vector<size_t> argvlen;

    argv.push_back("HMSET");
    argvlen.push_back(5);

    argv.push_back(key.c_str());
    argvlen.push_back(key.length());

    for (const auto& [field, value] : fields)
    {
        argv.push_back(field.c_str());
        argvlen.push_back(field.length());
        argv.push_back(value.c_str());
        argvlen.push_back(value.length());

        LOG_DEBUG( "[RedisStatePublisher]   %s = %s", field.c_str(), value.c_str());
    }

    redisReply* reply = (redisReply*)redisCommandArgv(stateDbContext_,
        argv.size(), argv.data(), argvlen.data());

    if (!reply)
    {
        LOG_ERROR( "[RedisStatePublisher] HMSET failed: connection lost (key=%s)", key.c_str());
        return false;
    }

    if (reply->type == REDIS_REPLY_ERROR)
    {
        LOG_ERROR( "[RedisStatePublisher] HMSET failed: %s (key=%s)", reply->str, key.c_str());
        freeReplyObject(reply);
        return false;
    }

    LOG_DEBUG( "[RedisStatePublisher] HMSET successful (key=%s, %zu fields)", key.c_str(), fields.size());
    freeReplyObject(reply);
    return true;
}

} // namespace sonic::dbus_bridge
