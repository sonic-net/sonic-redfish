///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2024 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

#include "platform_json_adapter.hpp"
#include "logger.hpp"
#include <json/json.h>
#include <fstream>
#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace sonic::dbus_bridge
{

PlatformJsonAdapter::PlatformJsonAdapter(const std::string& platformJsonPath)
    : platformJsonPath_(platformJsonPath)
{
}

bool PlatformJsonAdapter::load()
{
    std::string expandedPath = expandPath(platformJsonPath_);

    LOG_INFO("Loading platform.json from: %s", expandedPath.c_str());

    if (!parseJson(expandedPath))
    {
        LOG_WARNING("Failed to load platform.json, using defaults");
        return false;
    }

    loaded_ = true;
    return true;
}

std::string PlatformJsonAdapter::expandPath(const std::string& path) const
{
    std::string result = path;
    
    // Replace ${PLATFORM} with environment variable
    size_t pos = result.find("${PLATFORM}");
    if (pos != std::string::npos)
    {
        const char* platform = std::getenv("PLATFORM");
        if (platform)
        {
            result.replace(pos, 11, platform);
        }
        else
        {
            // Default platform for ASPEED
            LOG_DEBUG("PLATFORM is not set; using the default platform name");
            result.replace(pos, 11, "arm64-aspeed_ast2700-r0");
        }
    }
    
    return result;
}

bool PlatformJsonAdapter::parseJson(const std::string& path)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        LOG_WARNING("Cannot open %s: %s", path.c_str(), strerror(errno));
        return false;
    }

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errs;
    
    if (!Json::parseFromStream(builder, file, &root, &errs))
    {
        LOG_ERROR("JSON parse error: %s", errs.c_str());
        return false;
    }

    // Extract chassis name
    if (root.isMember("chassis") && root["chassis"].isMember("name"))
    {
        description_.chassisName = root["chassis"]["name"].asString();
    }

    // Extract chassis part number
    if (root.isMember("chassis") && root["chassis"].isMember("part_number"))
    {
        description_.chassisPartNumber = root["chassis"]["part_number"].asString();
    }

    // Extract chassis hardware version
    if (root.isMember("chassis") && root["chassis"].isMember("hardware_version"))
    {
        description_.chassisHardwareVersion = root["chassis"]["hardware_version"].asString();
    }

    // Extract PSU names
    if (root.isMember("chassis") && root["chassis"].isMember("psus"))
    {
        const Json::Value& psus = root["chassis"]["psus"];
        for (const auto& psu : psus)
        {
            if (psu.isMember("name"))
            {
                description_.psuNames.push_back(psu["name"].asString());
            }
        }
    }

    // Extract fan names
    if (root.isMember("chassis") && root["chassis"].isMember("fans"))
    {
        const Json::Value& fans = root["chassis"]["fans"];
        for (const auto& fan : fans)
        {
            if (fan.isMember("name"))
            {
                description_.fanNames.push_back(fan["name"].asString());
            }
        }
    }

    // Extract thermal names
    if (root.isMember("chassis") && root["chassis"].isMember("thermals"))
    {
        const Json::Value& thermals = root["chassis"]["thermals"];
        for (const auto& thermal : thermals)
        {
            if (thermal.isMember("name"))
            {
                description_.thermalNames.push_back(thermal["name"].asString());
            }
        }
    }

    LOG_INFO("platform.json loaded: %zu PSUs, %zu fans, %zu thermals",
             description_.psuNames.size(), description_.fanNames.size(),
             description_.thermalNames.size());
    return true;
}

PlatformDescription PlatformJsonAdapter::getPlatformDescription() const
{
    return description_;
}

std::optional<std::string> PlatformJsonAdapter::getChassisName() const
{
    if (!loaded_ || description_.chassisName.empty())
    {
        return std::nullopt;
    }
    return description_.chassisName;
}

std::optional<std::string> PlatformJsonAdapter::getChassisPartNumber() const
{
    if (!loaded_)
    {
        return std::nullopt;
    }
    return description_.chassisPartNumber;
}

std::optional<std::string> PlatformJsonAdapter::getChassisHardwareVersion() const
{
    if (!loaded_)
    {
        return std::nullopt;
    }
    return description_.chassisHardwareVersion;
}

} // namespace sonic::dbus_bridge

