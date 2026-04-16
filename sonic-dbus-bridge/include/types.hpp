///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2024 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
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

    // Source tracking
    FieldSource serialNumberSource{FieldSource::Default};
    FieldSource partNumberSource{FieldSource::Default};
    FieldSource manufacturerSource{FieldSource::Default};
    FieldSource modelSource{FieldSource::Default};
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
 */
struct LeakSensorInfo
{
    std::string name;              // e.g., "leak_sensor_1"
    std::string state{"OK"};       // "OK", "Warning", "Critical", "Unavailable", "Absent"
    std::string type{"Moisture"};  // "Moisture" or "FloatSwitch"
    bool present{false};
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

