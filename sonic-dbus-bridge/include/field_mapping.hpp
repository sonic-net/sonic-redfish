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
// Declarative regex mapping tables for Rack Manager telemetry and alerts.
// Patterns are matched case-insensitively as whole-name matches against a
// recursively-walked payload, so nesting depth and wrapper key names do not
// matter.  Telemetry flattens every match into a single Redis hash; alerts
// fan out into per-name RACK_MANAGER_ALERT|<name> keys.
//

#include <string>
#include <vector>

namespace sonic::dbus_bridge
{

enum class FieldType
{
    String,
    Number,
    Integer,
    Boolean
};

// Canonical Redis hash key into which all telemetry matches are flattened.
inline constexpr const char* TELEMETRY_KEY = "RSCM_TELEMETRY|alarms";

struct TelemetryRule
{
    std::string pathPattern;   // Regex vs. a leaf's trailing dotted path.
    std::string redisField;    // Flattened field name in TELEMETRY_KEY.
    FieldType   type;          // Expected value type (for serialisation).
};

// Telemetry rules (input: SubmitTelemetry "Alarms" envelope).  Keep redisField
// values unique and patterns mutually exclusive (first match wins).
inline const std::vector<TelemetryRule>& getTelemetryRules()
{
    static const std::vector<TelemetryRule> rules = {
        // --- Sensor status flags ---
        {".*EnergyValveActive",       "energy_valve_active",       FieldType::Boolean},
        {".*EnergyValvePresent",      "energy_valve_present",      FieldType::Boolean},
        {".*FlowrateSensorActive",    "flowrate_sensor_active",    FieldType::Boolean},
        {".*PressureSensorActive",    "pressure_sensor_active",    FieldType::Boolean},
        {".*TemperatureSensorActive", "temperature_sensor_active", FieldType::Boolean},

        // --- Inlet temperature deviation ---
        {".*InletTempDeviation\\.InletTemperature", "inlet_temp_deviation_temperature", FieldType::Number},
        {".*InletTempDeviation\\.Severity",         "inlet_temp_deviation_severity",    FieldType::String},

        // --- Flow rate deviation ---
        {".*FlowRateDeviation\\.FlowRate", "flow_rate_deviation_flow_rate", FieldType::Number},
        {".*FlowRateDeviation\\.Severity", "flow_rate_deviation_severity",  FieldType::String},

        // --- Liquid pressure deviation ---
        {".*LiquidPressureDeviation\\.LiquidPressure", "liquid_pressure_deviation_pressure", FieldType::Number},
        {".*LiquidPressureDeviation\\.Severity",       "liquid_pressure_deviation_severity", FieldType::String},

        // --- Leak detection ---
        {".*LeakDetected\\.LeakDetected",  "leak_detected",          FieldType::Boolean},
        {".*LeakDetected\\.LeakRopeBreak", "leak_rope_break",        FieldType::Boolean},
        {".*LeakDetected\\.Severity",      "leak_detected_severity", FieldType::String},

        // --- General ---
        {".*ThresholdConfigVersion", "threshold_config_version", FieldType::String},
        {".*GlycolConcentration",    "glycol_concentration",     FieldType::Number},
        {".*ErrorState",             "error_state",              FieldType::String},
        {".*RscmPosition",           "rscm_position",            FieldType::Integer},
        {".*ConfigFileCorrupted",    "config_file_corrupted",    FieldType::Boolean},
    };
    return rules;
}

// Canonical Redis key prefix for all alerts (STATE_DB).  Alerts arrive under a
// top-level envelope key matching "redfish.*" (e.g. "redfish_alert_data") and
// are matched by field/entry name; Severity / RscmPosition inherit from the
// nearest enclosing object.
inline constexpr const char* ALERT_KEY_PREFIX = "RACK_MANAGER_ALERT|";

struct AlertRule
{
    std::string fieldPattern;  // Regex vs. JSON field/entry name.
    std::string alertName;     // Canonical suffix -> RACK_MANAGER_ALERT|<alertName>
    std::string unit;          // Measurement unit (empty for leak rules)
    bool        isMeasurement; // true: numeric value+unit; false: stateful leak
};

// Alert rules.  Keep patterns mutually exclusive (first match wins).
inline const std::vector<AlertRule>& getAlertRules()
{
    static const std::vector<AlertRule> rules = {
        // --- Measurements (numeric fields; unified "value" field in Redis) ---
        {".*InletTemperature", "Inlet_liquid_temperature", "C",               true},
        {".*FlowRate",         "Inlet_liquid_flow_rate",   "gallons_per_min", true},
        {".*LiquidPressure",   "Inlet_liquid_pressure",    "psi",             true},

        // --- Stateful leak alerts (object entries; severity only) ---
        {".*LeakRopeBreak",    "leak_rope_break",          "",                false},
        {".*LeakDetected",     "leak_detected",            "",                false},
    };
    return rules;
}

} // namespace sonic::dbus_bridge
