///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2024 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

#include "inventory_model.hpp"
#include "logger.hpp"

namespace sonic::dbus_bridge
{

// Helper function to convert FieldSource to string
static const char* fieldSourceToString(FieldSource source)
{
    switch (source)
    {
        case FieldSource::Redis: return "Redis";
        case FieldSource::FruEeprom: return "FRU EEPROM";
        case FieldSource::PlatformJson: return "platform.json";
        case FieldSource::Default: return "Default";
        default: return "Unknown";
    }
}

InventoryModel InventoryModelBuilder::build(
    const std::optional<FruInfo>& fruInfo,
    const std::optional<DeviceMetadata>& deviceMetadata,
    const std::optional<PlatformDescription>& platformDesc,
    const std::optional<ChassisState>& chassisState)
{
    InventoryModel model;
    
    model.chassis = buildChassisInfo(fruInfo, deviceMetadata, platformDesc);
    model.system = buildSystemInfo(fruInfo, deviceMetadata);
    model.chassisState = chassisState.value_or(ChassisState{});
    model.psus = buildPsuList(platformDesc);
    model.fans = buildFanList(platformDesc);
    
    return model;
}

ChassisInfo InventoryModelBuilder::buildChassisInfo(
    const std::optional<FruInfo>& fruInfo,
    const std::optional<DeviceMetadata>& deviceMetadata,
    const std::optional<PlatformDescription>& platformDesc)
{
    ChassisInfo chassis;

    // Serial number: Redis > FRU > platform.json > default
    if (deviceMetadata && deviceMetadata->serialNumber)
    {
        chassis.serialNumber = *deviceMetadata->serialNumber;
        chassis.serialNumberSource = FieldSource::Redis;
    }
    else if (fruInfo && fruInfo->serialNumber)
    {
        chassis.serialNumber = *fruInfo->serialNumber;
        chassis.serialNumberSource = FieldSource::FruEeprom;
    }
    // No platform.json fallback for serial number

    // Part number: Redis > FRU > platform.json > hwsku > default
    if (deviceMetadata && deviceMetadata->partNumber)
    {
        chassis.partNumber = *deviceMetadata->partNumber;
        chassis.partNumberSource = FieldSource::Redis;
    }
    else if (fruInfo && fruInfo->partNumber)
    {
        chassis.partNumber = *fruInfo->partNumber;
        chassis.partNumberSource = FieldSource::FruEeprom;
    }
    else if (platformDesc && platformDesc->chassisPartNumber)
    {
        chassis.partNumber = *platformDesc->chassisPartNumber;
        chassis.partNumberSource = FieldSource::PlatformJson;
    }
    else if (deviceMetadata && deviceMetadata->hwsku)
    {
        chassis.partNumber = *deviceMetadata->hwsku;
        chassis.partNumberSource = FieldSource::Redis;
    }

    // Manufacturer: Redis > FRU > platform.json > default
    if (deviceMetadata && deviceMetadata->manufacturer)
    {
        chassis.manufacturer = *deviceMetadata->manufacturer;
        chassis.manufacturerSource = FieldSource::Redis;
    }
    else if (fruInfo && fruInfo->manufacturer)
    {
        chassis.manufacturer = *fruInfo->manufacturer;
        chassis.manufacturerSource = FieldSource::FruEeprom;
    }
    // No platform.json fallback for manufacturer

    // Model: Redis model > Redis platform > FRU > platform.json > default
    if (deviceMetadata && deviceMetadata->model)
    {
        chassis.model = *deviceMetadata->model;
        chassis.modelSource = FieldSource::Redis;
    }
    else if (deviceMetadata && deviceMetadata->platform)
    {
        chassis.model = *deviceMetadata->platform;
        chassis.modelSource = FieldSource::Redis;
    }
    else if (fruInfo && fruInfo->model)
    {
        chassis.model = *fruInfo->model;
        chassis.modelSource = FieldSource::FruEeprom;
    }
    // No platform.json fallback for model

    // Hardware version: Redis > FRU > platform.json > default
    if (fruInfo && fruInfo->hardwareVersion)
    {
        chassis.hardwareVersion = *fruInfo->hardwareVersion;
    }
    else if (platformDesc && platformDesc->chassisHardwareVersion)
    {
        chassis.hardwareVersion = *platformDesc->chassisHardwareVersion;
    }

    // Chassis Type: Redis > default
    if (deviceMetadata && deviceMetadata->type)
    {
        chassis.chassisType = *deviceMetadata->type;
    }

    // Pretty name: platform.json > FRU product name > default
    if (platformDesc && !platformDesc->chassisName.empty())
    {
        chassis.prettyName = platformDesc->chassisName;
    }
    else if (fruInfo && fruInfo->productName)
    {
        chassis.prettyName = *fruInfo->productName;
    }

    // Log data sources
    LOG_INFO("Chassis Data Sources:");
    LOG_INFO("  SerialNumber: \"%s\" (from %s)",
           chassis.serialNumber.c_str(), fieldSourceToString(chassis.serialNumberSource));
    LOG_INFO("  PartNumber: \"%s\" (from %s)",
           chassis.partNumber.c_str(), fieldSourceToString(chassis.partNumberSource));
    LOG_INFO("  Manufacturer: \"%s\" (from %s)",
           chassis.manufacturer.c_str(), fieldSourceToString(chassis.manufacturerSource));
    LOG_INFO("  Model: \"%s\" (from %s)",
           chassis.model.c_str(), fieldSourceToString(chassis.modelSource));

    return chassis;
}

SystemInfo InventoryModelBuilder::buildSystemInfo(
    const std::optional<FruInfo>& fruInfo,
    const std::optional<DeviceMetadata>& deviceMetadata)
{
    SystemInfo system;
    
    // Serial number: FRU > CONFIG_DB > default
    if (fruInfo && fruInfo->serialNumber)
    {
        system.serialNumber = *fruInfo->serialNumber;
    }
    else if (deviceMetadata && deviceMetadata->serialNumber)
    {
        system.serialNumber = *deviceMetadata->serialNumber;
    }
    
    // Manufacturer: FRU > CONFIG_DB > default
    if (fruInfo && fruInfo->manufacturer)
    {
        system.manufacturer = *fruInfo->manufacturer;
    }
    else if (deviceMetadata && deviceMetadata->manufacturer)
    {
        system.manufacturer = *deviceMetadata->manufacturer;
    }
    
    // Model: FRU > CONFIG_DB > default
    if (fruInfo && fruInfo->model)
    {
        system.model = *fruInfo->model;
    }
    else if (deviceMetadata && deviceMetadata->platform)
    {
        system.model = *deviceMetadata->platform;
    }
    
    // Hostname: CONFIG_DB > default
    if (deviceMetadata && deviceMetadata->hostname)
    {
        system.hostname = *deviceMetadata->hostname;
        system.prettyName = *deviceMetadata->hostname;
    }
    
    return system;
}

std::vector<PsuInfo> InventoryModelBuilder::buildPsuList(
    const std::optional<PlatformDescription>& platformDesc)
{
    std::vector<PsuInfo> psus;
    
    if (platformDesc)
    {
        for (const auto& name : platformDesc->psuNames)
        {
            PsuInfo psu;
            psu.name = name;
            psu.present = false; // Will be updated by sensors later
            psus.push_back(psu);
        }
    }
    
    return psus;
}

std::vector<FanInfo> InventoryModelBuilder::buildFanList(
    const std::optional<PlatformDescription>& platformDesc)
{
    std::vector<FanInfo> fans;
    
    if (platformDesc)
    {
        for (const auto& name : platformDesc->fanNames)
        {
            FanInfo fan;
            fan.name = name;
            fan.present = false; // Will be updated by sensors later
            fans.push_back(fan);
        }
    }
    
    return fans;
}

bool hasChanged(const InventoryModel& oldModel, const InventoryModel& newModel)
{
    // Compare chassis info
    if (oldModel.chassis.serialNumber != newModel.chassis.serialNumber ||
        oldModel.chassis.partNumber != newModel.chassis.partNumber ||
        oldModel.chassis.manufacturer != newModel.chassis.manufacturer ||
        oldModel.chassis.model != newModel.chassis.model)
    {
        return true;
    }
    
    // Compare system info
    if (oldModel.system.serialNumber != newModel.system.serialNumber ||
        oldModel.system.manufacturer != newModel.system.manufacturer ||
        oldModel.system.model != newModel.system.model ||
        oldModel.system.hostname != newModel.system.hostname)
    {
        return true;
    }
    
    // Compare chassis state
    if (oldModel.chassisState.powerState != newModel.chassisState.powerState)
    {
        return true;
    }
    
    return false;
}

} // namespace sonic::dbus_bridge

