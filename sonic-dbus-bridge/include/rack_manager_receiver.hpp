///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2026 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

#pragma once

#include "field_mapping.hpp"

#include <hiredis/hiredis.h>
#include <sdbusplus/asio/object_server.hpp>

#include <memory>
#include <string>
#include <vector>

namespace sonic::dbus_bridge
{

// D-Bus constants for rack manager receiver
constexpr const char* RACK_MANAGER_OBJ_PATH = "/xyz/openbmc_project/sonic/rack_manager";
constexpr const char* RACK_MANAGER_IFACE    = "com.sonic.RackManager";

/**
 * @brief Receives alert / telemetry JSON from bmcweb via D-Bus and
 *        persists the data to Redis using the declarative field-mapping tables.
 *
 * D-Bus interface: com.sonic.RackManager
 *   Methods:
 *     SubmitAlert(s json)     -> b success
 *     SubmitTelemetry(s json) -> b success
 */
class RackManagerReceiver
{
  public:
    /**
     * @param server  Object server to register the D-Bus interface on
     * @param redisHost  STATE_DB Redis host
     * @param redisPort  STATE_DB Redis port
     */
    RackManagerReceiver(sdbusplus::asio::object_server& server,
                        const std::string& redisHost = "localhost",
                        int redisPort = 6379);
    ~RackManagerReceiver();

    // non-copyable
    RackManagerReceiver(const RackManagerReceiver&) = delete;
    RackManagerReceiver& operator=(const RackManagerReceiver&) = delete;

    /**
     * @brief Register D-Bus methods and connect to Redis.
     * @return true on success
     */
    bool initialize();

  private:
    sdbusplus::asio::object_server& server_;
    std::shared_ptr<sdbusplus::asio::dbus_interface> iface_;

    std::string redisHost_;
    int redisPort_;
    redisContext* redisCtx_;

    bool connectRedis();

    /**
     * @brief Walk the mapping table, extract values from JSON, store in Redis.
     */
    bool storeFields(const std::string& jsonStr,
                     const std::vector<FieldMapping>& mappings);

    bool hset(const std::string& key, const std::string& field,
              const std::string& value);
};

} // namespace sonic::dbus_bridge
