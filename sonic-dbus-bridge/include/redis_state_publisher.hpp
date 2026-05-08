///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2024 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

#pragma once

#include <hiredis/hiredis.h>
#include <string>
#include <map>
#include <mutex>
#include <cstdint>

namespace sonic::dbus_bridge
{

/**
 * @brief Redis State Publisher for BMC host state management
 *
 * Publishes power commands to RACK_MANAGER_COMMAND and host state to
 * HOST_STATE|switch-host in STATE_DB.
 */
class RedisStatePublisher
{
public:
    RedisStatePublisher();
    ~RedisStatePublisher();
    
    // Disable copy
    RedisStatePublisher(const RedisStatePublisher&) = delete;
    RedisStatePublisher& operator=(const RedisStatePublisher&) = delete;
    
    /**
     * @brief Connect to Redis STATE_DB (DB 6)
     * 
     * @param host Redis host (default: localhost)
     * @param port Redis port (default: 6379)
     * @return true if connection successful
     */
    bool connect(const std::string& host = "localhost", int port = 6379);
    
    /**
     * @brief Check if connected to Redis
     */
    bool isConnected() const { return stateDbContext_ != nullptr; }
    
    /**
     * @brief Publish a power command to RACK_MANAGER_COMMAND.
     *
     *   Key: RACK_MANAGER_COMMAND|<command_id>
     *   Fields:
     *     - command:               POWER_ON | POWER_OFF | POWER_CYCLE | GRACEFUL_SHUT
     *     - status:                PENDING
     *     - result:                ""
     *     - last_change_timestamp: ISO 8601 UTC
     *
     * @param command Command name from the vocabulary above
     * @return command_id on success, empty string on failure
     */
    std::string publishHostRequest(const std::string& command);

    /**
     * @brief Update the HOST_STATE|switch-host hash in STATE_DB.
     *
     * @param devicePowerState  e.g. "POWERED_ON", "POWERING_ON", "POWERED_OFF"
     * @param deviceStatus      "ONLINE" or "OFFLINE"
     * @return true if update successful
     */
    bool updateHostState(const std::string& devicePowerState,
                         const std::string& deviceStatus);

    /**
     * @brief Update RACK_MANAGER_COMMAND status field for a previously
     *        published command.
     *
     * Key: RACK_MANAGER_COMMAND|<commandId>
     *
     * @param commandId Command id returned from publishHostRequest
     * @param status    New status (PENDING|IN_PROGRESS|DONE|FAILED)
     * @return true if update successful
     */
    bool updateRequestStatus(const std::string& commandId,
                             const std::string& status);
    
private:
    redisContext* stateDbContext_;
    std::mutex redisMutex_;  // Protect Redis operations
    uint64_t requestCounter_;
    
    /**
     * @brief Generate unique command id
     * Format: CMD_<unix_seconds>_<counter>
     */
    std::string generateRequestId();
    
    /**
     * @brief Set a single field in a Redis hash
     */
    bool hset(const std::string& key, const std::string& field, const std::string& value);
    
    /**
     * @brief Set multiple fields in a Redis hash
     */
    bool hmset(const std::string& key, const std::map<std::string, std::string>& fields);
};

} // namespace sonic::dbus_bridge

