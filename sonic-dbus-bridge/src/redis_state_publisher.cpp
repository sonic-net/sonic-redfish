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
#include <ctime>

namespace sonic::dbus_bridge
{

namespace
{

constexpr const char* TABLE_RACK_MANAGER_COMMAND = "RACK_MANAGER_COMMAND";
constexpr const char* KEY_HOST_STATE             = "HOST_STATE|switch-host";
constexpr const char* CMD_STATUS_PENDING         = "PENDING";

// ISO 8601 UTC timestamp with microsecond precision.
std::string isoUtcNow()
{
    auto now      = std::chrono::system_clock::now();
    auto secs     = std::chrono::system_clock::to_time_t(now);
    auto usPart   = std::chrono::duration_cast<std::chrono::microseconds>(
                        now.time_since_epoch()).count() % 1'000'000;
    std::tm tm{};
    gmtime_r(&secs, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S")
        << '.' << std::setw(6) << std::setfill('0') << usPart << 'Z';
    return oss.str();
}

} // namespace

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
    oss << "CMD_" << timestamp << "_"
        << std::setfill('0') << std::setw(6) << requestCounter_;

    std::string requestId = oss.str();
    LOG_DEBUG( "[RedisStatePublisher] Generated command id: %s", requestId.c_str());

    return requestId;
}

std::string RedisStatePublisher::publishHostRequest(const std::string& command)
{
    LOG_INFO( "[RedisStatePublisher] ========================================");
    LOG_INFO( "[RedisStatePublisher] Publishing RACK_MANAGER_COMMAND");
    LOG_INFO( "[RedisStatePublisher] command: %s", command.c_str());

    std::string commandId = generateRequestId();
    std::string timestamp = isoUtcNow();

    std::map<std::string, std::string> fields = {
        {"command",               command},
        {"status",                CMD_STATUS_PENDING},
        {"result",                ""},
        {"last_change_timestamp", timestamp}
    };

    LOG_INFO( "[RedisStatePublisher] Command details:");
    LOG_INFO( "[RedisStatePublisher]   - command_id:            %s", commandId.c_str());
    LOG_INFO( "[RedisStatePublisher]   - command:               %s", command.c_str());
    LOG_INFO( "[RedisStatePublisher]   - status:                %s", CMD_STATUS_PENDING);
    LOG_INFO( "[RedisStatePublisher]   - last_change_timestamp: %s", timestamp.c_str());

    std::string redisKey =
        std::string(TABLE_RACK_MANAGER_COMMAND) + "|" + commandId;

    if (!hmset(redisKey, fields))
    {
        LOG_ERROR( "[RedisStatePublisher] Failed to publish RACK_MANAGER_COMMAND to Redis");
        LOG_ERROR( "[RedisStatePublisher] ========================================");
        return "";
    }

    LOG_INFO( "[RedisStatePublisher] Published successfully to %s", redisKey.c_str());
    LOG_INFO( "[RedisStatePublisher] ========================================");

    return commandId;
}

bool RedisStatePublisher::updateHostState(const std::string& devicePowerState,
                                          const std::string& deviceStatus)
{
    LOG_INFO( "[RedisStatePublisher] ========================================");
    LOG_INFO( "[RedisStatePublisher] Updating %s", KEY_HOST_STATE);
    LOG_INFO( "[RedisStatePublisher]   - device_power_state: %s", devicePowerState.c_str());
    LOG_INFO( "[RedisStatePublisher]   - device_status:      %s", deviceStatus.c_str());

    std::map<std::string, std::string> fields = {
        {"device_power_state",    devicePowerState},
        {"device_status",         deviceStatus},
        {"last_change_timestamp", isoUtcNow()}
    };

    bool result = hmset(KEY_HOST_STATE, fields);

    if (result)
    {
        LOG_INFO( "[RedisStatePublisher] %s updated successfully", KEY_HOST_STATE);
    }
    else
    {
        LOG_ERROR( "[RedisStatePublisher] Failed to update %s", KEY_HOST_STATE);
    }

    LOG_INFO( "[RedisStatePublisher] ========================================");
    return result;
}

bool RedisStatePublisher::updateRequestStatus(const std::string& commandId,
                                              const std::string& status)
{
    LOG_INFO( "[RedisStatePublisher] Updating RACK_MANAGER_COMMAND status");
    LOG_INFO( "[RedisStatePublisher]   - command_id: %s", commandId.c_str());
    LOG_INFO( "[RedisStatePublisher]   - status:     %s", status.c_str());

    std::string redisKey =
        std::string(TABLE_RACK_MANAGER_COMMAND) + "|" + commandId;

    std::map<std::string, std::string> fields = {
        {"status",                status},
        {"last_change_timestamp", isoUtcNow()}
    };

    bool result = hmset(redisKey, fields);

    if (result)
    {
        LOG_INFO( "[RedisStatePublisher] Status updated for %s", redisKey.c_str());
    }
    else
    {
        LOG_ERROR( "[RedisStatePublisher] Failed to update status for %s", redisKey.c_str());
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
