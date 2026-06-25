///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2026 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

#pragma once

//
// Header-only helpers shared by the Rack Manager receiver's alert and
// telemetry parsers.  These are pure, side-effect-free utilities (timestamp
// formatting, numeric value rendering, whole-name regex matching) kept out
// of the translation unit so the parsing logic stays focused on the domain
// rules.
//

#include "logger.hpp"

#include <json/json.h>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <regex>
#include <sstream>
#include <string>

namespace sonic::dbus_bridge
{

// ISO 8601 UTC timestamp with microsecond precision, matching the format
// used for RACK_MANAGER_COMMAND.last_change_timestamp (see
// redis_state_publisher.cpp).
inline std::string isoUtcNow()
{
    auto now    = std::chrono::system_clock::now();
    auto secs   = std::chrono::system_clock::to_time_t(now);
    auto usPart = std::chrono::duration_cast<std::chrono::microseconds>(
                      now.time_since_epoch()).count() % 1'000'000;
    std::tm tm{};
    gmtime_r(&secs, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S")
        << '.' << std::setw(6) << std::setfill('0') << usPart << 'Z';
    return oss.str();
}

// Render a numeric Json::Value as the telemetry value string.
inline std::string numberToString(const Json::Value& val)
{
    if (val.isDouble())   { return std::to_string(val.asDouble()); }
    if (val.isIntegral()) { return std::to_string(val.asInt64()); }
    return "";
}

// Whole-name regex match (anchored via regex_match).  Case-insensitive by
// default, as used for the field-driven alert/telemetry rules; pass
// caseInsensitive=false for fixed, case-sensitive patterns (e.g. the
// "redfish.*" alert envelope).
inline bool matchesName(const std::string& name, const std::string& pattern,
                        bool caseInsensitive = true)
{
    try
    {
        auto flags = std::regex::ECMAScript;
        if (caseInsensitive)
        {
            flags |= std::regex::icase;
        }
        std::regex re(pattern, flags);
        return std::regex_match(name, re);
    }
    catch (const std::regex_error& e)
    {
        LOG_ERROR("RackManagerReceiver: bad match pattern '%s': %s",
                  pattern.c_str(), e.what());
        return false;
    }
}

} // namespace sonic::dbus_bridge
