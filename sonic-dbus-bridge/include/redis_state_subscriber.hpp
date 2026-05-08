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
#include <thread>
#include <atomic>
#include <functional>
#include <map>
#include <vector>

namespace sonic::dbus_bridge
{

/**
 * @brief Redis State Subscriber using keyspace notifications
 *
 * Subscribes to Redis keyspace notifications for multiple keys
 * and invokes callbacks when any subscribed key is updated.
 */
class RedisStateSubscriber
{
public:
    /**
     * @brief Callback function type
     * Parameters: (key, field, value)
     * - key: Redis key that changed (e.g., "HOST_STATE|switch-host", "DEVICE_METADATA")
     * - field: Hash field that changed (e.g., "device_power_state", "serial_number")
     * - value: New value of the field
     */
    using KeyspaceCallback = std::function<void(const std::string&,
                                                 const std::string&,
                                                 const std::string&)>;

    RedisStateSubscriber();
    ~RedisStateSubscriber();

    // Disable copy
    RedisStateSubscriber(const RedisStateSubscriber&) = delete;
    RedisStateSubscriber& operator=(const RedisStateSubscriber&) = delete;

    /**
     * @brief Start subscriber thread (single key - backward compatible)
     *
     * Connects to Redis STATE_DB and subscribes to keyspace notifications
     * for a single key.
     *
     * @param host Redis host
     * @param port Redis port
     * @param callback Function to call when state changes
     * @return true if started successfully
     */
    bool start(const std::string& host, int port, KeyspaceCallback callback);

    /**
     * @brief Start subscriber thread (multiple keys)
     *
     * Connects to Redis STATE_DB and subscribes to keyspace notifications
     * for multiple keys.
     *
     * @param host Redis host
     * @param port Redis port
     * @param keys List of Redis keys to subscribe to
     * @param callback Function to call when any key changes
     * @return true if started successfully
     */
    bool startMultiKey(const std::string& host, int port,
                       const std::vector<std::string>& keys,
                       KeyspaceCallback callback);
    
    /**
     * @brief Stop subscriber thread
     */
    void stop();
    
    /**
     * @brief Check if subscriber is running
     */
    bool isRunning() const { return running_; }
    
private:
    redisContext* subContext_;
    redisContext* getContext_;  // Separate context for HGETALL
    std::thread subscriberThread_;
    std::atomic<bool> running_;
    KeyspaceCallback callback_;
    
    /**
     * @brief Subscriber thread main loop
     */
    void subscriberLoop();
    
    /**
     * @brief Handle keyspace notification
     * @param channel Channel name (e.g., "__keyspace@6__:HOST_STATE|switch-host")
     */
    void handleKeyspaceNotification(const std::string& channel);
    
    /**
     * @brief Get all fields from a hash
     * @param key Redis key
     * @return Map of field->value
     */
    std::map<std::string, std::string> hgetall(const std::string& key);
};

} // namespace sonic::dbus_bridge

