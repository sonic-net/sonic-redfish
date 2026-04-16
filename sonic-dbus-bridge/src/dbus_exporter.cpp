///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2024 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

#include "dbus_exporter.hpp"
#include "logger.hpp"

namespace sonic::dbus_bridge
{

// D-Bus interface names (OpenBMC standard)
constexpr const char* IFACE_INVENTORY_CHASSIS = "xyz.openbmc_project.Inventory.Item.Chassis";
constexpr const char* IFACE_DECORATOR_ASSET = "xyz.openbmc_project.Inventory.Decorator.Asset";
constexpr const char* IFACE_DECORATOR_MODEL = "xyz.openbmc_project.Inventory.Decorator.Model";
constexpr const char* IFACE_STATE_CHASSIS = "xyz.openbmc_project.State.Chassis";
constexpr const char* IFACE_SOFTWARE_VERSION = "xyz.openbmc_project.Software.Version";
constexpr const char* IFACE_SOFTWARE_ACTIVATION = "xyz.openbmc_project.Software.Activation";
constexpr const char* IFACE_LEAK_DETECTOR = "xyz.openbmc_project.Inventory.Item.LeakDetector";
constexpr const char* IFACE_OPERATIONAL_STATUS = "xyz.openbmc_project.State.Decorator.OperationalStatus";
constexpr const char* IFACE_INVENTORY_ITEM = "xyz.openbmc_project.Inventory.Item";

constexpr const char* LEAK_SENSOR_BASE_PATH = "/xyz/openbmc_project/sensors/leak/";
constexpr const char* DETECTOR_STATE_PREFIX =
    "xyz.openbmc_project.Inventory.Item.LeakDetector.DetectorState.";

// D-Bus object paths
constexpr const char* OBJ_PATH_CHASSIS = "/xyz/openbmc_project/inventory/system/chassis";
constexpr const char* OBJ_PATH_SYSTEM = "/xyz/openbmc_project/inventory/system/system0";
constexpr const char* OBJ_PATH_CHASSIS_STATE = "/xyz/openbmc_project/state/chassis0";

DBusExporter::DBusExporter(sdbusplus::asio::object_server& inventoryServer)
    : inventoryServer_(inventoryServer)
{
}

bool DBusExporter::createObjects(const InventoryModel& model)
{
    LOG_INFO("Creating D-Bus objects...");

    if (!createChassisObject(model.chassis))
    {
        LOG_ERROR("Failed to create chassis object");
        return false;
    }

    if (!createSystemObject(model.system))
    {
        LOG_ERROR("Failed to create system object");
        return false;
    }

    if (!createChassisStateObject(model.chassisState))
    {
        LOG_ERROR("Failed to create chassis state object");
        return false;
    }

    if (!model.firmwareVersions.empty())
    {
        if (!createFirmwareObjects(model.firmwareVersions))
        {
            LOG_WARNING("Failed to create some firmware inventory objects");
        }
    }

    if (!model.leakSensors.empty())
    {
        if (!createLeakSensorObjects(model.leakSensors))
        {
            LOG_WARNING("Failed to create some leak sensor objects");
        }
    }

    currentModel_ = model;

    LOG_INFO("D-Bus objects created successfully");
    return true;
}

bool DBusExporter::updateObjects(const InventoryModel& model)
{
    // For now, just update the model
    // Property change signals would be emitted here
    currentModel_ = model;
    return true;
}

bool DBusExporter::createChassisObject(const ChassisInfo& chassis)
{
    // Store chassis data in currentModel_ for property getters
    currentModel_.chassis = chassis;

    // Item.Chassis interface (REQUIRED by bmcweb for chassis discovery!)
    auto chassisIface = inventoryServer_.add_interface(OBJ_PATH_CHASSIS, IFACE_INVENTORY_CHASSIS);
    chassisIface->register_property_r<std::string>(
        "Type", std::string(""),
        sdbusplus::vtable::property_::const_,
        [](const auto&) {
            return "xyz.openbmc_project.Inventory.Item.Chassis.ChassisType.RackMount";
        });
    chassisIface->initialize();
    interfaces_[std::string(OBJ_PATH_CHASSIS) + ":" + IFACE_INVENTORY_CHASSIS] = chassisIface;

    // Decorator.Asset interface
    auto assetIface = inventoryServer_.add_interface(OBJ_PATH_CHASSIS, IFACE_DECORATOR_ASSET);
    assetIface->register_property_r<std::string>(
        "SerialNumber", std::string(""),
        sdbusplus::vtable::property_::const_,
        [this](const auto&) { return currentModel_.chassis.serialNumber; });
    assetIface->register_property_r<std::string>(
        "PartNumber", std::string(""),
        sdbusplus::vtable::property_::const_,
        [this](const auto&) { return currentModel_.chassis.partNumber; });
    assetIface->register_property_r<std::string>(
        "Manufacturer", std::string(""),
        sdbusplus::vtable::property_::const_,
        [this](const auto&) { return currentModel_.chassis.manufacturer; });
    assetIface->initialize();
    interfaces_[std::string(OBJ_PATH_CHASSIS) + ":" + IFACE_DECORATOR_ASSET] = assetIface;

    // Decorator.Model interface
    auto modelIface = inventoryServer_.add_interface(OBJ_PATH_CHASSIS, IFACE_DECORATOR_MODEL);
    modelIface->register_property_r<std::string>(
        "Model", std::string(""),
        sdbusplus::vtable::property_::const_,
        [this](const auto&) { return currentModel_.chassis.model; });
    modelIface->initialize();
    interfaces_[std::string(OBJ_PATH_CHASSIS) + ":" + IFACE_DECORATOR_MODEL] = modelIface;

    LOG_INFO("Created chassis object at %s", OBJ_PATH_CHASSIS);
    return true;
}

bool DBusExporter::createSystemObject(const SystemInfo& system)
{
    // Store system data in currentModel_ for property getters
    currentModel_.system = system;

    // Decorator.Asset interface
    auto assetIface = inventoryServer_.add_interface(OBJ_PATH_SYSTEM, IFACE_DECORATOR_ASSET);
    assetIface->register_property_r<std::string>(
        "SerialNumber", std::string(""),
        sdbusplus::vtable::property_::const_,
        [this](const auto&) { return currentModel_.system.serialNumber; });
    assetIface->register_property_r<std::string>(
        "Manufacturer", std::string(""),
        sdbusplus::vtable::property_::const_,
        [this](const auto&) { return currentModel_.system.manufacturer; });
    assetIface->initialize();
    interfaces_[std::string(OBJ_PATH_SYSTEM) + ":" + IFACE_DECORATOR_ASSET] = assetIface;

    // Decorator.Model interface
    auto modelIface = inventoryServer_.add_interface(OBJ_PATH_SYSTEM, IFACE_DECORATOR_MODEL);
    modelIface->register_property_r<std::string>(
        "Model", std::string(""),
        sdbusplus::vtable::property_::const_,
        [this](const auto&) { return currentModel_.system.model; });
    modelIface->initialize();
    interfaces_[std::string(OBJ_PATH_SYSTEM) + ":" + IFACE_DECORATOR_MODEL] = modelIface;

    LOG_INFO("Created system object at %s", OBJ_PATH_SYSTEM);
    return true;
}

bool DBusExporter::createChassisStateObject(const ChassisState& state)
{
    // Store state data in currentModel_ for property getters
    currentModel_.chassisState = state;

    // State.Chassis interface
    auto stateIface = inventoryServer_.add_interface(OBJ_PATH_CHASSIS_STATE, IFACE_STATE_CHASSIS);
    stateIface->register_property_r<std::string>(
        "CurrentPowerState", std::string(""),
        sdbusplus::vtable::property_::const_,
        [this](const auto&) {
            return (currentModel_.chassisState.powerState == "on")
                       ? "xyz.openbmc_project.State.Chassis.PowerState.On"
                       : "xyz.openbmc_project.State.Chassis.PowerState.Off";
        });
    stateIface->initialize();
    interfaces_[std::string(OBJ_PATH_CHASSIS_STATE) + ":" + IFACE_STATE_CHASSIS] = stateIface;

    LOG_INFO("Created chassis state object at %s", OBJ_PATH_CHASSIS_STATE);
    return true;
}

bool DBusExporter::createFirmwareObjects(
    const std::vector<FirmwareVersionInfo>& versions)
{
    currentModel_.firmwareVersions = versions;

    for (size_t i = 0; i < versions.size(); i++)
    {
        const auto& fw = versions[i];
        std::string objPath = "/xyz/openbmc_project/software/" + fw.id;

        try
        {
            std::string purposeStr;
            switch (fw.purpose)
            {
                case FirmwarePurpose::BMC:
                    purposeStr = "xyz.openbmc_project.Software.Version.VersionPurpose.BMC";
                    break;
                case FirmwarePurpose::Host:
                    purposeStr = "xyz.openbmc_project.Software.Version.VersionPurpose.Host";
                    break;
                default:
                    purposeStr = "xyz.openbmc_project.Software.Version.VersionPurpose.Other";
                    break;
            }

            // Software.Version interface
            auto versionIface = inventoryServer_.add_interface(objPath,
                                                                IFACE_SOFTWARE_VERSION);
            versionIface->register_property_r<std::string>(
                "Version", std::string(""),
                sdbusplus::vtable::property_::const_,
                [this, idx = i](const auto&) {
                    if (idx < currentModel_.firmwareVersions.size())
                    {
                        return currentModel_.firmwareVersions[idx].version;
                    }
                    return std::string("Unknown");
                });
            versionIface->register_property_r<std::string>(
                "Purpose", std::string(""),
                sdbusplus::vtable::property_::const_,
                [purposeStr](const auto&) {
                    return purposeStr;
                });
            versionIface->initialize();
            interfaces_[objPath + ":" + IFACE_SOFTWARE_VERSION] = versionIface;

            // Software.Activation interface
            auto activationIface = inventoryServer_.add_interface(objPath,
                                                                   IFACE_SOFTWARE_ACTIVATION);
            activationIface->register_property_r<std::string>(
                "Activation", std::string(""),
                sdbusplus::vtable::property_::const_,
                [](const auto&) {
                    return std::string(
                        "xyz.openbmc_project.Software.Activation.Activations.Active");
                });
            activationIface->register_property_r<std::string>(
                "RequestedActivation", std::string(""),
                sdbusplus::vtable::property_::const_,
                [](const auto&) {
                    return std::string(
                        "xyz.openbmc_project.Software.Activation.RequestedActivations.None");
                });
            activationIface->initialize();
            interfaces_[objPath + ":" + IFACE_SOFTWARE_ACTIVATION] = activationIface;

            LOG_INFO("Created firmware object at %s (version=%s, purpose=%s)",
                     objPath.c_str(), fw.version.c_str(), purposeStr.c_str());
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("Failed to create firmware object at %s: %s",
                      objPath.c_str(), e.what());
        }
    }

    return true;
}

bool DBusExporter::createLeakSensorObjects(
    const std::vector<LeakSensorInfo>& sensors)
{
    currentModel_.leakSensors = sensors;

    for (const auto& sensor : sensors)
    {
        std::string objPath = LEAK_SENSOR_BASE_PATH + sensor.name;

        try
        {
            // Map state string to fully-qualified D-Bus enum
            std::string stateEnum = DETECTOR_STATE_PREFIX + sensor.state;

            // LeakDetector interface — DetectorState is mutable so set_property()
            // emits PropertiesChanged signals for bmcweb event monitoring.
            auto leakIface = inventoryServer_.add_interface(objPath,
                                                             IFACE_LEAK_DETECTOR);
            leakIface->register_property<std::string>("DetectorState", stateEnum);
            leakIface->register_property_r<std::string>(
                "Type", std::string(""),
                sdbusplus::vtable::property_::const_,
                [type = sensor.type](const auto&) {
                    return type;
                });
            leakIface->initialize();
            interfaces_[objPath + ":" + IFACE_LEAK_DETECTOR] = leakIface;

            // OperationalStatus interface
            bool functional = (sensor.state == "OK");
            auto statusIface = inventoryServer_.add_interface(objPath,
                                                               IFACE_OPERATIONAL_STATUS);
            statusIface->register_property<bool>("Functional", functional);
            statusIface->initialize();
            interfaces_[objPath + ":" + IFACE_OPERATIONAL_STATUS] = statusIface;

            // Inventory.Item interface
            auto itemIface = inventoryServer_.add_interface(objPath,
                                                             IFACE_INVENTORY_ITEM);
            itemIface->register_property_r<bool>(
                "Present", sensor.present,
                sdbusplus::vtable::property_::const_,
                [present = sensor.present](const auto&) {
                    return present;
                });
            itemIface->register_property_r<std::string>(
                "PrettyName", std::string(""),
                sdbusplus::vtable::property_::const_,
                [name = sensor.name](const auto&) {
                    return std::string("Leak Detector ") + name;
                });
            itemIface->initialize();
            interfaces_[objPath + ":" + IFACE_INVENTORY_ITEM] = itemIface;

            LOG_INFO("Created leak sensor object at %s (state=%s, type=%s)",
                     objPath.c_str(), sensor.state.c_str(), sensor.type.c_str());
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("Failed to create leak sensor object at %s: %s",
                      objPath.c_str(), e.what());
        }
    }

    return true;
}

bool DBusExporter::updateLeakSensorState(const std::string& sensorName,
                                          const std::string& newState)
{
    std::string objPath = LEAK_SENSOR_BASE_PATH + sensorName;
    std::string leakKey = objPath + ":" + IFACE_LEAK_DETECTOR;
    std::string statusKey = objPath + ":" + IFACE_OPERATIONAL_STATUS;

    auto leakIt = interfaces_.find(leakKey);
    if (leakIt == interfaces_.end())
    {
        LOG_ERROR("Leak sensor interface not found for %s", sensorName.c_str());
        return false;
    }

    std::string stateEnum = DETECTOR_STATE_PREFIX + newState;

    // set_property emits PropertiesChanged D-Bus signal
    bool ok = leakIt->second->set_property("DetectorState", stateEnum);
    if (!ok)
    {
        LOG_ERROR("Failed to set DetectorState for %s", sensorName.c_str());
        return false;
    }

    // Update Functional status
    auto statusIt = interfaces_.find(statusKey);
    if (statusIt != interfaces_.end())
    {
        bool functional = (newState == "OK");
        statusIt->second->set_property("Functional", functional);
    }

    // Update cached model
    for (auto& s : currentModel_.leakSensors)
    {
        if (s.name == sensorName)
        {
            s.state = newState;
            break;
        }
    }

    LOG_INFO("Updated leak sensor %s state to %s", sensorName.c_str(),
             newState.c_str());
    return true;
}

} // namespace sonic::dbus_bridge

