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
#include <sdbusplus/asio/object_server.hpp>
#include <memory>
#include <map>
#include <string>

namespace sonic::dbus_bridge
{

/**
 * @brief D-Bus object exporter
 *
 * Creates and manages D-Bus objects that bmcweb expects:
 * - /xyz/openbmc_project/inventory/system/chassis
 * - /xyz/openbmc_project/inventory/system/system0
 * - /xyz/openbmc_project/state/chassis0
 * - /xyz/openbmc_project/software/<id>
 */
class DBusExporter
{
  public:
    /**
     * @brief Construct a new DBus Exporter
     *
     * @param inventoryServer Object server for inventory objects (chassis, system, state)
     */
    explicit DBusExporter(sdbusplus::asio::object_server& inventoryServer);

    /**
     * @brief Destructor - cleanup is automatic (RAII)
     */
    ~DBusExporter() = default;

    /**
     * @brief Create all D-Bus objects from inventory model
     *
     * @param model Complete inventory model
     * @return true on success, false on error
     */
    bool createObjects(const InventoryModel& model);

    /**
     * @brief Update D-Bus objects with new model
     *
     * Only updates properties that have changed
     *
     * @param model New inventory model
     * @return true on success, false on error
     */
    bool updateObjects(const InventoryModel& model);

    /**
     * @brief Update a leak sensor's DetectorState on D-Bus
     *
     * Calls set_property() which emits a PropertiesChanged D-Bus signal,
     * allowing bmcweb to generate Redfish events.
     *
     * @param sensorName Sensor name (e.g., "leak_sensor_1")
     * @param newState New state string ("OK", "Warning", "Critical", etc.)
     * @return true on success
     */
    bool updateLeakSensorState(const std::string& sensorName,
                                const std::string& newState);

  private:
    sdbusplus::asio::object_server& inventoryServer_;

    // Current model (for change detection)
    InventoryModel currentModel_;

    // D-Bus interfaces (managed by shared_ptr, cleanup is automatic)
    std::map<std::string, std::shared_ptr<sdbusplus::asio::dbus_interface>> interfaces_;

    /**
     * @brief Create chassis inventory object
     */
    bool createChassisObject(const ChassisInfo& chassis);

    /**
     * @brief Create system inventory object
     */
    bool createSystemObject(const SystemInfo& system);

    /**
     * @brief Create chassis state object
     */
    bool createChassisStateObject(const ChassisState& state);

    /**
     * @brief Create firmware version objects under /xyz/openbmc_project/software/
     *
     * Creates one D-Bus object per firmware entry with:
     * - xyz.openbmc_project.Software.Version (Purpose, Version)
     * - xyz.openbmc_project.Software.Activation (Activation, RequestedActivation)
     */
    bool createFirmwareObjects(const std::vector<FirmwareVersionInfo>& versions);

    /**
     * @brief Create leak sensor D-Bus objects under /xyz/openbmc_project/sensors/leak/
     *
     * Creates one D-Bus object per leak sensor with:
     * - xyz.openbmc_project.Inventory.Item.LeakDetector (Type, DetectorState)
     * - xyz.openbmc_project.State.Decorator.OperationalStatus (Functional)
     * - xyz.openbmc_project.Inventory.Item (Present, PrettyName)
     *
     * DetectorState is registered as a mutable property so that set_property()
     * emits PropertiesChanged signals for bmcweb event monitoring.
     */
    bool createLeakSensorObjects(const std::vector<LeakSensorInfo>& sensors);
};

} // namespace sonic::dbus_bridge

