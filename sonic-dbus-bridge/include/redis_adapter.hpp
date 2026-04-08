///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2024 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

#pragma once

#include "types.hpp"
#include <hiredis/hiredis.h>
#include <memory>
#include <string>
#include <map>
#include <optional>

namespace sonic::dbus_bridge
{

/**
 * @brief Redis client adapter for SONiC databases
 * 
 * Connects to CONFIG_DB (DB 4) and STATE_DB (DB 6) and provides
 * methods to read device metadata and chassis state.
 */
class RedisAdapter
{
  public:
    /**
     * @brief Construct a new Redis Adapter
     * 
     * @param configDbHost CONFIG_DB host (default: localhost)
     * @param configDbPort CONFIG_DB port (default: 6379)
     * @param stateDbHost STATE_DB host (default: localhost)
     * @param stateDbPort STATE_DB port (default: 6379)
     */
    RedisAdapter(const std::string& configDbHost = "localhost",
                 int configDbPort = 6379,
                 const std::string& stateDbHost = "localhost",
                 int stateDbPort = 6379);

    ~RedisAdapter();

    // Disable copy
    RedisAdapter(const RedisAdapter&) = delete;
    RedisAdapter& operator=(const RedisAdapter&) = delete;

    /**
     * @brief Connect to Redis databases
     * 
     * @return true if at least one database connected successfully
     * @return false if all connections failed
     */
    bool connect();

    /**
     * @brief Check if CONFIG_DB is connected
     */
    bool isConfigDbConnected() const { return configDbContext_ != nullptr; }

    /**
     * @brief Check if STATE_DB is connected
     */
    bool isStateDbConnected() const { return stateDbContext_ != nullptr; }

    /**
     * @brief Get device metadata from CONFIG_DB
     * 
     * Reads DEVICE_METADATA|localhost hash
     * 
     * @return DeviceMetadata structure (fields may be empty if unavailable)
     */
    DeviceMetadata getDeviceMetadata();

    /**
     * @brief Get chassis state from STATE_DB
     * 
     * Reads CHASSIS_STATE_TABLE|chassis0 hash
     * 
     * @return ChassisState structure (defaults to "on" if unavailable)
     */
    ChassisState getChassisState();

    /**
     * @brief Get firmware versions from STATE_DB
     *
     * Reads BMC_FW_INVENTORY|* keys from STATE_DB on the switch
     * Returns placeholder "N/A" for entries
     * not yet published.
     *
     * @return Vector of firmware version entries
     */
    std::vector<FirmwareVersionInfo> getFirmwareVersions();

    /**
     * @brief Get all leak sensor data from STATE_DB
     *
     * Reads LEAK_SENSOR|* keys from STATE_DB.
     * Each key is a hash with fields: state, type, present.
     *
     * @return Vector of leak sensor entries
     */
    std::vector<LeakSensorInfo> getLeakSensors();

    /**
     * @brief Get a single leak sensor by name from STATE_DB
     *
     * Reads LEAK_SENSOR|<name> hash. More efficient than getLeakSensors()
     * when only one sensor needs to be read (avoids KEYS scan).
     *
     * @param name Sensor name (e.g., "leak_sensor_1")
     * @return Sensor info if found, nullopt otherwise
     */
    std::optional<LeakSensorInfo> getLeakSensor(const std::string& name);

  private:
    std::string configDbHost_;
    int configDbPort_;
    std::string stateDbHost_;
    int stateDbPort_;

    redisContext* configDbContext_{nullptr};
    redisContext* stateDbContext_{nullptr};

    /**
     * @brief Connect to a specific Redis database
     * 
     * @param host Redis host
     * @param port Redis port
     * @param dbIndex Database index (4 for CONFIG_DB, 6 for STATE_DB)
     * @return redisContext* on success, nullptr on failure
     */
    redisContext* connectToDb(const std::string& host, int port, int dbIndex);

    /**
     * @brief Get all fields from a Redis hash
     * 
     * @param ctx Redis context
     * @param key Hash key
     * @return Map of field->value, empty if key doesn't exist
     */
    std::map<std::string, std::string> hgetall(redisContext* ctx,
                                                const std::string& key);

    /**
     * @brief Get a single field from a Redis hash
     * 
     * @param ctx Redis context
     * @param key Hash key
     * @param field Field name
     * @return Field value if exists, nullopt otherwise
     */
    std::optional<std::string> hget(redisContext* ctx,
                                    const std::string& key,
                                    const std::string& field);

    /**
     * @brief Find keys matching a pattern
     *
     * @param ctx Redis context
     * @param pattern Glob-style pattern (e.g., "LEAK_SENSOR|*")
     * @return Vector of matching key names
     */
    std::vector<std::string> keys(redisContext* ctx,
                                   const std::string& pattern);

    /**
     * @brief Free Redis reply
     */
    void freeReply(void* reply);
};

} // namespace sonic::dbus_bridge

