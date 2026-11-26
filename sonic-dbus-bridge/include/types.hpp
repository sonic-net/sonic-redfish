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

#include <optional>
#include <string>
#include <vector>
#include <map>

namespace sonic::dbus_bridge
{

/**
 * @brief FRU (Field Replaceable Unit) information from EEPROM
 */
struct FruInfo
{
    std::optional<std::string> serialNumber;
    std::optional<std::string> partNumber;
    std::optional<std::string> manufacturer;
    std::optional<std::string> model;
    std::optional<std::string> hardwareVersion;
    std::optional<std::string> manufactureDate;
    std::optional<std::string> productName;
};

/**
 * @brief Device metadata from CONFIG_DB
 */
struct DeviceMetadata
{
    std::optional<std::string> platform;
    std::optional<std::string> hwsku;
    std::optional<std::string> hostname;
    std::optional<std::string> mac;
    std::optional<std::string> type;
    std::optional<std::string> manufacturer;
    std::optional<std::string> serialNumber;
    std::optional<std::string> partNumber;
    std::optional<std::string> model;
};

/**
 * @brief Chassis state from STATE_DB
 */
struct ChassisState
{
    std::string powerState{"on"}; // "on" or "off"
};

/**
 * @brief Switch-Host state from STATE_DB (HOST_STATE|switch-host, owned by bmcctld)
 */
struct HostStateInfo
{
    std::string devicePowerState;    // POWERED_ON, POWERING_OFF, ... (see host_state_mapping.hpp)
    std::string deviceStatus;        // "ONLINE" or "OFFLINE"
    std::string lastChangeTimestamp; // ISO 8601 UTC
};

/**
 * @brief Rack manager command record from STATE_DB (RACK_MANAGER_COMMAND|<id>)
 */
struct RackManagerCommandInfo
{
    std::string commandId;           // CMD_<unix_seconds>_<counter>
    std::string command;             // POWER_ON | POWER_OFF | GRACEFUL_SHUT | POWER_CYCLE
    std::string status;              // PENDING | IN_PROGRESS | DONE | FAILED
    std::string result;              // SUCCESS or failure reason (set by bmcctld)
    std::string lastChangeTimestamp; // ISO 8601 UTC
};

/**
 * @brief Data source for a field
 */
enum class FieldSource
{
    Redis,
    FruEeprom,
    PlatformJson,
    Default
};

/**
 * @brief Normalized chassis information
 */
struct ChassisInfo
{
    std::string serialNumber{"Unknown"};
    std::string partNumber{"Unknown"};
    std::string manufacturer{"Unknown"};
    std::string model{"Unknown"};
    std::string hardwareVersion{"Unknown"};
    std::string chassisType{"RackMount"};
    bool present{true};
    std::string prettyName{"SONiC Chassis"};
    // Base MAC address from CONFIG_DB, stored lower-cased; empty if CONFIG_DB
    // does not provide it.
    std::string baseMacAddress{""};

    // Source tracking
    FieldSource serialNumberSource{FieldSource::Default};
    FieldSource partNumberSource{FieldSource::Default};
    FieldSource manufacturerSource{FieldSource::Default};
    FieldSource modelSource{FieldSource::Default};
    FieldSource baseMacAddressSource{FieldSource::Default};
};

/**
 * @brief Normalized system information
 */
struct SystemInfo
{
    std::string serialNumber{"Unknown"};
    std::string manufacturer{"Unknown"};
    std::string model{"Unknown"};
    std::string hostname{"sonic"};
    bool present{true};
    std::string prettyName{"SONiC System"};
};

/**
 * @brief PSU information
 */
struct PsuInfo
{
    std::string name;
    std::string serialNumber{"Unknown"};
    std::string model{"Unknown"};
    bool present{false};
};

/**
 * @brief Fan information
 */
struct FanInfo
{
    std::string name;
    bool present{false};
};

/**
 * @brief Leak sensor information from STATE_DB
 *
 * Mirrors the LIQUID_COOLING_INFO|<sensor_name> schema populated by
 * thermalctld.
 */
struct LeakSensorInfo
{
    std::string name;                     // e.g., "leakage1"
    std::string leaking{"N/A"};           // "Yes", "No", "N/A" (sensor not readable)
    std::string leakSensorStatus{"Good"}; // "Good" or "Fault"
    std::string leakSeverity{"None"};     // "CRITICAL", "MINOR", "None" (per-sensor)
    std::string type{"unknown"};          // e.g., "rope", "flex_pcb", "spot"
    std::string location{"unknown"};      // leak sensor location

    /**
     * @brief Derive the D-Bus/Redfish DetectorState from the STATE_DB fields
     *
     * "OK" when not leaking, "Warning" for a MINOR leak, "Critical" for any
     * other confirmed leak, "Unavailable" when the sensor is not readable.
     * A faulted sensor cannot vouch for "not leaking", so that is
     * "Unavailable" too; a leak it still reports is kept.
     */
    std::string detectorState() const
    {
        if (leaking == "No")
        {
            return functional() ? "OK" : "Unavailable";
        }
        if (leaking == "Yes")
        {
            return (leakSeverity == "MINOR") ? "Warning" : "Critical";
        }
        return "Unavailable";
    }

    /**
     * @brief Is the leak sensor hardware itself healthy?
     */
    bool functional() const
    {
        return leakSensorStatus != "Fault";
    }

    /**
     * @brief Map the platform sensor type onto the Redfish LeakDetectorType
     *
     * Redfish only defines "Moisture" and "FloatSwitch"; the platform
     * sensor types (rope, flex_pcb, spot, ...) are all moisture based.
     */
    std::string redfishDetectorType() const
    {
        return (type.find("float") != std::string::npos) ? "FloatSwitch"
                                                         : "Moisture";
    }
};

/**
 * @brief Platform description from platform.json
 */
struct PlatformDescription
{
    std::string chassisName;
    std::optional<std::string> chassisPartNumber;
    std::optional<std::string> chassisHardwareVersion;
    std::vector<std::string> fanNames;
    std::vector<std::string> psuNames;
    std::vector<std::string> thermalNames;
};

/**
 * @brief Firmware version purpose
 */
enum class FirmwarePurpose
{
    BMC,
    Host,
    Other
};

/**
 * @brief Firmware version information for FirmwareInventory
 */
struct FirmwareVersionInfo
{
    std::string id;       // Unique identifier
    std::string version;  // Version string
    FirmwarePurpose purpose{FirmwarePurpose::Other};
};

/**
 * @brief Complete inventory model
 */
struct InventoryModel
{
    ChassisInfo chassis;
    SystemInfo system;
    ChassisState chassisState;
    std::vector<PsuInfo> psus;
    std::vector<FanInfo> fans;
    std::vector<LeakSensorInfo> leakSensors;
    std::vector<FirmwareVersionInfo> firmwareVersions;
};

/**
 * @brief Data source health status
 */
enum class DataSourceHealth
{
    Healthy,
    Degraded,
    Unavailable
};

/**
 * @brief Data source types
 */
enum class DataSource
{
    RedisConfigDb,
    RedisStateDb,
    PlatformJson,
    FruEeprom
};

} // namespace sonic::dbus_bridge

