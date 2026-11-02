///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2024 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// Author: Chinmoy Dey <chinmoy@nexthop.ai>
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

#pragma once

#include "types.hpp"
#include <sdbusplus/asio/object_server.hpp>
#include <functional>
#include <memory>
#include <map>
#include <string>
#include <vector>

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
    /// Registers an object created after startup with the ObjectMapper.
    using ObjectRegistrar = std::function<void(
        const std::string& path, const std::vector<std::string>& interfaces)>;

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
     * @brief Update a leak sensor's DetectorState and Functional on D-Bus
     *
     * Derives DetectorState from the leaking/leak_severity fields and
     * Functional from leak_sensor_status. Calls set_property() which emits
     * a PropertiesChanged D-Bus signal, allowing bmcweb to generate
     * Redfish events.
     *
     * @param sensor Refreshed leak sensor data from STATE_DB
     * @return true on success
     */
    bool updateLeakSensorState(const LeakSensorInfo& sensor);

    /**
     * @brief Set the callback that registers runtime-created objects
     *
     * Objects created at startup are registered by BridgeApp. Objects
     * created later (leak sensors that appear after startup) are passed
     * to this callback so bmcweb can discover them.
     */
    void setObjectRegistrar(ObjectRegistrar registrar)
    {
        objectRegistrar_ = std::move(registrar);
    }

    /**
     * @brief Is there a D-Bus object for this leak sensor?
     */
    bool hasLeakSensor(const std::string& name) const;

    /**
     * @brief Create the D-Bus object for a leak sensor seen after startup
     *
     * thermalctld can publish LIQUID_COOLING_INFO|<name> after the bridge
     * has read STATE_DB. Creates the object, adds it to the model,
     * registers it through the ObjectRegistrar and, when the sensor is not
     * OK, emits PropertiesChanged for DetectorState so bmcweb reports the
     * state it was first seen in.
     *
     * @param sensor Leak sensor data from STATE_DB
     * @return true if the object exists after the call
     */
    bool addLeakSensorObject(const LeakSensorInfo& sensor);

  private:
    sdbusplus::asio::object_server& inventoryServer_;

    // Current model (for change detection)
    InventoryModel currentModel_;

    // D-Bus interfaces (managed by shared_ptr, cleanup is automatic)
    std::map<std::string, std::shared_ptr<sdbusplus::asio::dbus_interface>> interfaces_;

    ObjectRegistrar objectRegistrar_;

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

    /**
     * @brief Create the D-Bus object for one leak sensor
     *
     * @return true on success
     */
    bool createLeakSensorObject(const LeakSensorInfo& sensor);
};

} // namespace sonic::dbus_bridge

