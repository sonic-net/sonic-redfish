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
// Declarative regex mapping table for Rack Manager telemetry and alerts.
// A single SensorRule table maps a measurement-field / leak-entry name to the
// canonical sensor name used in the STATE_DB key, matching the platform DB
// schema (see pmon-bmc-design.md, section 2.1.2.1 "DB schema").  Both
// SubmitTelemetry and SubmitAlert fan out into per-sensor keys:
//   RACK_MANAGER_DATA|<SensorName>   (telemetry: value/unit/severity/timestamp)
//   RACK_MANAGER_ALERT|<SensorName>  (alert: severity/timestamp)
// Patterns are matched case-insensitively as whole-name matches against a
// recursively-walked payload, so nesting depth and wrapper key names do not
// matter.
//

#include <string>
#include <vector>

namespace sonic::dbus_bridge
{

// Canonical Redis key prefixes (STATE_DB) for the per-sensor records.
inline constexpr const char* DATA_KEY_PREFIX  = "RACK_MANAGER_DATA|";
inline constexpr const char* ALERT_KEY_PREFIX = "RACK_MANAGER_ALERT|";

enum class SensorKind
{
    Measurement,  // numeric leaf -> telemetry stores value+unit+severity+ts
    Leak          // stateful entry -> stores leak(=severity)+timestamp
};

struct SensorRule
{
    std::string fieldPattern;  // Regex vs. JSON measurement-field / leak-entry name.
    std::string sensorName;    // Canonical -> RACK_MANAGER_{DATA,ALERT}|<sensorName>
    std::string unit;          // Telemetry measurement unit (empty for leak).
    std::string valueField;    // Telemetry value field name (empty for leak).
    SensorKind  kind;
};

// Sensor rules aligned to the platform DB schema.  Keep patterns mutually
// exclusive (first match wins) and sensorName values unique.  Per the doc the
// telemetry temperature record names its value field "InletTemperature" while
// flow-rate and pressure use the generic "value".
inline const std::vector<SensorRule>& getSensorRules()
{
    static const std::vector<SensorRule> rules = {
        // --- Measurements (numeric fields) ---
        {".*InletTemperature", "Inlet_liquid_temperature", "C",               "InletTemperature", SensorKind::Measurement},
        {".*FlowRate",         "Inlet_liquid_flow_rate",   "gallons_per_min", "value",            SensorKind::Measurement},
        {".*LiquidPressure",   "Inlet_liquid_pressure",    "psi",             "value",            SensorKind::Measurement},

        // --- Stateful leak (collapsed into a single Rack_level_leak record) ---
        {".*Leak.*",           "Rack_level_leak",          "",                "",                 SensorKind::Leak},
    };
    return rules;
}

} // namespace sonic::dbus_bridge
