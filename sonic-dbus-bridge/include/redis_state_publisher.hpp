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
 * Publishes host transition requests to Redis STATE_DB and updates
 * the SWITCH_HOST_STATE table. Uses hiredis for Redis communication.
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
     * @brief Publish a host transition request to Redis
     *
     * Creates an entry in BMC_HOST_REQUEST table with SONiC key format:
     * Redis key: BMC_HOST_REQUEST|<request_id>
     * Fields:
     * - requested_transition: the transition type
     * - request_id: unique identifier
     * - timestamp: current time
     * - status: "pending"
     *
     * @param transition Transition type (e.g., "reset-in", "reset-out", "reset-cycle")
     * @return request_id on success, empty string on failure
     */
    std::string publishHostRequest(const std::string& transition);
    
    /**
     * @brief Update SWITCH_HOST_STATE table
     * 
     * @param deviceState "POWERED_ON" or "POWERED_OFF"
     * @param deviceStatus "REACHABLE" or "UNREACHABLE"
     * @return true if update successful
     */
    bool updateSwitchHostState(const std::string& deviceState,
                               const std::string& deviceStatus);
    
    /**
     * @brief Update BMC_HOST_REQUEST status field
     *
     * Updates the status field for the given request using SONiC key format:
     * Redis key: BMC_HOST_REQUEST|<requestId>
     *
     * @param requestId Request ID to update (used to construct the Redis key)
     * @param status New status ("pending", "processing", "completed", "failed")
     * @return true if update successful
     */
    bool updateRequestStatus(const std::string& requestId,
                            const std::string& status);
    
private:
    redisContext* stateDbContext_;
    std::mutex redisMutex_;  // Protect Redis operations
    uint64_t requestCounter_;
    
    /**
     * @brief Generate unique request ID
     * Format: req_<timestamp>_<counter>
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

