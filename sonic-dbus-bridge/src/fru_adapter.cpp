///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2024 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

#include "fru_adapter.hpp"
#include "logger.hpp"
#include <fstream>
#include <cerrno>
#include <cstring>
#include <cstdint>

namespace sonic::dbus_bridge
{

// ONIE TLV type codes
constexpr uint8_t TLV_CODE_PRODUCT_NAME = 0x21;
constexpr uint8_t TLV_CODE_PART_NUMBER = 0x22;
constexpr uint8_t TLV_CODE_SERIAL_NUMBER = 0x23;
constexpr uint8_t TLV_CODE_MAC_BASE = 0x24;
constexpr uint8_t TLV_CODE_MANUFACTURE_DATE = 0x25;
constexpr uint8_t TLV_CODE_DEVICE_VERSION = 0x26;
constexpr uint8_t TLV_CODE_PLATFORM_NAME = 0x28;
constexpr uint8_t TLV_CODE_ONIE_VERSION = 0x29;
constexpr uint8_t TLV_CODE_MANUFACTURER = 0x2B;
constexpr uint8_t TLV_CODE_COUNTRY_CODE = 0x2C;
constexpr uint8_t TLV_CODE_VENDOR = 0x2D;
constexpr uint8_t TLV_CODE_MODEL = 0x2E;
constexpr uint8_t TLV_CODE_CRC32 = 0xFE;

FruAdapter::FruAdapter(const std::vector<std::string>& eepromPaths)
    : eepromPaths_(eepromPaths)
{
}

bool FruAdapter::scanAndLoad()
{
    LOG_INFO("Scanning for FRU EEPROMs...");

    for (const auto& path : eepromPaths_)
    {
        LOG_DEBUG("Trying FRU EEPROM: %s", path.c_str());
        if (readEeprom(path))
        {
            LOG_INFO("Successfully read FRU from: %s", path.c_str());
            loaded_ = true;
            return true;
        }
    }

    LOG_WARNING("No FRU EEPROM found");
    return false;
}

bool FruAdapter::readEeprom(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        // Several candidate paths are tried, so a miss is a debug line.
        LOG_DEBUG("Cannot open FRU EEPROM %s: %s", path.c_str(),
                  strerror(errno));
        return false;
    }

    // Read entire EEPROM
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
    
    if (data.size() < 11) // Minimum TLV header size
    {
        LOG_DEBUG("FRU EEPROM %s is too short (%zu bytes)", path.c_str(),
                  data.size());
        return false;
    }

    // Validate TLV header
    if (data[0] != 'T' || data[1] != 'l' || data[2] != 'v' ||
        data[3] != 'I' || data[4] != 'n' || data[5] != 'f' ||
        data[6] != 'o' || data[7] != 0x00)
    {
        LOG_DEBUG("Invalid TLV header in EEPROM");
        return false;
    }

    // Parse TLV data
    fruInfo_ = parseTlv(data);
    return true;
}

FruInfo FruAdapter::parseTlv(const std::vector<uint8_t>& data)
{
    FruInfo info;
    
    // Skip header (8 bytes) and version (1 byte)
    size_t offset = 9;

    // Total length is at bytes 9-10 (big-endian)
    uint16_t totalLen = (data[9] << 8) | data[10];
    offset = 11;

    while (offset < data.size() && offset < (11 + static_cast<size_t>(totalLen)))
    {
        if (offset + 2 > data.size())
        {
            LOG_WARNING("FRU TLV data ends inside a record header");
            break;
        }
        
        uint8_t type = data[offset];
        uint8_t len = data[offset + 1];
        offset += 2;
        
        if (offset + len > data.size())
        {
            LOG_WARNING("FRU TLV record 0x%02x runs past the end of the data",
                        type);
            break;
        }
        
        std::string value(reinterpret_cast<const char*>(&data[offset]), len);
        
        switch (type)
        {
            case TLV_CODE_PRODUCT_NAME:
                info.productName = value;
                break;
            case TLV_CODE_PART_NUMBER:
                info.partNumber = value;
                break;
            case TLV_CODE_SERIAL_NUMBER:
                info.serialNumber = value;
                break;
            case TLV_CODE_MANUFACTURE_DATE:
                info.manufactureDate = value;
                break;
            case TLV_CODE_DEVICE_VERSION:
                info.hardwareVersion = value;
                break;
            case TLV_CODE_MANUFACTURER:
                info.manufacturer = value;
                break;
            case TLV_CODE_MODEL:
                info.model = value;
                break;
            case TLV_CODE_CRC32:
                // End of TLV
                return info;
            default:
                // Unknown type, skip
                break;
        }
        
        offset += len;
    }
    
    return info;
}

bool FruAdapter::validateCrc(const std::vector<uint8_t>& /* data */)
{
    // TODO: Implement CRC32 validation
    // For MVP, skip CRC validation
    return true;
}

} // namespace sonic::dbus_bridge

