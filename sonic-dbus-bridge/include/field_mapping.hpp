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
// Declarative field-mapping tables for Rack Manager alerts and telemetry.
//
// Each entry maps a dot-separated JSON path to a Redis hash key+field.
// When the input JSON schema changes, update ONLY these tables -- no other
// code needs to change.
//
// Example (telemetry):
//   JSON path  "Alarms.InletTempDeviation.InletTemperature"
//   is stored as
//     HSET RSCM_TELEMETRY|alarms inlet_temp_deviation_temperature <value>
//
// Example (alert, flat):
//   JSON path  "Redfish.LiquidPressureDeviation.LiquidPressure"
//   is stored as
//     HSET RSCM_ALERT|LiquidPressureDeviation liquid_pressure <value>
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

struct FieldMapping
{
    std::string jsonPath;     // Dot-separated path in the input JSON
    std::string redisKey;     // Redis hash key
    std::string redisField;   // Redis hash field name
    FieldType   type;         // Expected value type (for serialisation)
};

// -----------------------------------------------------------------------
// Telemetry mappings  (input: SubmitTelemetry JSON)
//
// Body envelope:  {"Alarms": { ... }}
//
// To add a new field:
//   1. Add a line here with the JSON path, target Redis key/field, and type.
//   2. That's it -- the receiver walks this table automatically.
//
// To remove a field:
//   1. Delete the row.  The bridge will silently ignore the data if the
//      firmware still emits it (resolveJsonPath returns null for unknown
//      paths).  Stored history is not deleted by this change.
// -----------------------------------------------------------------------
inline const std::vector<FieldMapping>& getTelemetryMappings()
{
    static const std::vector<FieldMapping> mappings = {
        // --- Sensor status flags ---
        {"Alarms.EnergyValveActive",       "RSCM_TELEMETRY|alarms", "energy_valve_active",       FieldType::Boolean},
        {"Alarms.EnergyValvePresent",      "RSCM_TELEMETRY|alarms", "energy_valve_present",      FieldType::Boolean},
        {"Alarms.FlowrateSensorActive",    "RSCM_TELEMETRY|alarms", "flowrate_sensor_active",    FieldType::Boolean},
        {"Alarms.PressureSensorActive",    "RSCM_TELEMETRY|alarms", "pressure_sensor_active",    FieldType::Boolean},
        {"Alarms.TemperatureSensorActive", "RSCM_TELEMETRY|alarms", "temperature_sensor_active", FieldType::Boolean},

        // --- Inlet temperature deviation ---
        {"Alarms.InletTempDeviation.InletTemperature", "RSCM_TELEMETRY|alarms", "inlet_temp_deviation_temperature", FieldType::Number},
        {"Alarms.InletTempDeviation.Severity",         "RSCM_TELEMETRY|alarms", "inlet_temp_deviation_severity",    FieldType::String},

        // --- Flow rate deviation ---
        {"Alarms.FlowRateDeviation.FlowRate", "RSCM_TELEMETRY|alarms", "flow_rate_deviation_flow_rate", FieldType::Number},
        {"Alarms.FlowRateDeviation.Severity", "RSCM_TELEMETRY|alarms", "flow_rate_deviation_severity",  FieldType::String},

        // --- Liquid pressure deviation ---
        {"Alarms.LiquidPressureDeviation.LiquidPressure", "RSCM_TELEMETRY|alarms", "liquid_pressure_deviation_pressure", FieldType::Number},
        {"Alarms.LiquidPressureDeviation.Severity",       "RSCM_TELEMETRY|alarms", "liquid_pressure_deviation_severity", FieldType::String},

        // --- Leak detection ---
        {"Alarms.LeakDetected.LeakDetected",  "RSCM_TELEMETRY|alarms", "leak_detected",          FieldType::Boolean},
        {"Alarms.LeakDetected.LeakRopeBreak", "RSCM_TELEMETRY|alarms", "leak_rope_break",        FieldType::Boolean},
        {"Alarms.LeakDetected.Severity",      "RSCM_TELEMETRY|alarms", "leak_detected_severity", FieldType::String},

        // --- General ---
        {"Alarms.ThresholdConfigVersion", "RSCM_TELEMETRY|alarms", "threshold_config_version", FieldType::String},
        {"Alarms.GlycolConcentration",    "RSCM_TELEMETRY|alarms", "glycol_concentration",     FieldType::Number},
        {"Alarms.ErrorState",             "RSCM_TELEMETRY|alarms", "error_state",              FieldType::String},
        {"Alarms.RscmPosition",           "RSCM_TELEMETRY|alarms", "rscm_position",            FieldType::Integer},
        {"Alarms.ConfigFileCorrupted",    "RSCM_TELEMETRY|alarms", "config_file_corrupted",    FieldType::Boolean},
    };
    return mappings;
}

// -----------------------------------------------------------------------
// Alert mappings  (input: SubmitAlert JSON)
//
// Body envelope:  {"Redfish": { ... }}
//
// Two structural variants are supported:
//
//   FLAT form:
//     {"Redfish": {
//        "Alerts":                    { combined deviation + RscmPosition },
//        "LiquidPressureDeviation":   { ... },
//        "InletTemperatureDeviation": { ... },
//        "LeakDetected":              { ... },
//        "LeakRopeBreak":             { ... }
//     }}
//     Each child persists under RSCM_ALERT|<child-key>.
//
//   WRAPPED form (ShutdownAlert):
//     {"Redfish": {
//        "ShutdownAlert": {
//            "FlowRateDeviation":      {...},
//            "TempDeviation":          {...},
//            "LiquidPressureDeviation":{...},
//            "LeakDetected":           {...},
//            "LeakRopeBreak":          {...},
//            "RscmPosition": <int>   // wrapper-level, applies to every leaf
//        }
//     }}
//     Each leaf persists under RSCM_ALERT|ShutdownAlert|<Leaf>.
//     The wrapper's RscmPosition lands under RSCM_ALERT|ShutdownAlert;
//     readers must join the wrapper key with each leaf key to recover
//     the full context of a wrapped alert.
// -----------------------------------------------------------------------
inline const std::vector<FieldMapping>& getAlertMappings()
{
    static const std::vector<FieldMapping> mappings = {
        // -------------------------------------------------------------------
        // FLAT form (Sample 1)
        // -------------------------------------------------------------------

        // --- "Alerts": combined inlet temperature + flow rate alert ---
        {"Redfish.Alerts.InletTemperature", "RSCM_ALERT|Alerts", "inlet_temperature", FieldType::Number},
        {"Redfish.Alerts.FlowRate",         "RSCM_ALERT|Alerts", "flow_rate",         FieldType::Number},
        {"Redfish.Alerts.Severity",         "RSCM_ALERT|Alerts", "severity",          FieldType::String},
        {"Redfish.Alerts.RscmPosition",     "RSCM_ALERT|Alerts", "rscm_position",     FieldType::Integer},

        // --- LiquidPressureDeviation ---
        {"Redfish.LiquidPressureDeviation.LiquidPressure", "RSCM_ALERT|LiquidPressureDeviation", "liquid_pressure", FieldType::Number},
        {"Redfish.LiquidPressureDeviation.Severity",       "RSCM_ALERT|LiquidPressureDeviation", "severity",        FieldType::String},
        {"Redfish.LiquidPressureDeviation.RscmPosition",   "RSCM_ALERT|LiquidPressureDeviation", "rscm_position",   FieldType::Integer},

        // --- InletTemperatureDeviation ---
        {"Redfish.InletTemperatureDeviation.InletTemperature", "RSCM_ALERT|InletTemperatureDeviation", "inlet_temperature", FieldType::Number},
        {"Redfish.InletTemperatureDeviation.Severity",         "RSCM_ALERT|InletTemperatureDeviation", "severity",          FieldType::String},
        {"Redfish.InletTemperatureDeviation.RscmPosition",     "RSCM_ALERT|InletTemperatureDeviation", "rscm_position",     FieldType::Integer},

        // --- LeakDetected (alert form -- no inner LeakDetected bool) ---
        {"Redfish.LeakDetected.Severity",     "RSCM_ALERT|LeakDetected", "severity",      FieldType::String},
        {"Redfish.LeakDetected.RscmPosition", "RSCM_ALERT|LeakDetected", "rscm_position", FieldType::Integer},

        // --- LeakRopeBreak ---
        {"Redfish.LeakRopeBreak.Severity",     "RSCM_ALERT|LeakRopeBreak", "severity",      FieldType::String},
        {"Redfish.LeakRopeBreak.RscmPosition", "RSCM_ALERT|LeakRopeBreak", "rscm_position", FieldType::Integer},

        // -------------
        // WRAPPED form 
        // -------------

        // wrapper-level RscmPosition (applies to every leaf below)
        {"Redfish.ShutdownAlert.RscmPosition", "RSCM_ALERT|ShutdownAlert", "rscm_position", FieldType::Integer},

        // FlowRateDeviation (wrapped)
        {"Redfish.ShutdownAlert.FlowRateDeviation.FlowRate", "RSCM_ALERT|ShutdownAlert|FlowRateDeviation", "flow_rate", FieldType::Number},
        {"Redfish.ShutdownAlert.FlowRateDeviation.Severity", "RSCM_ALERT|ShutdownAlert|FlowRateDeviation", "severity",  FieldType::String},

        // TempDeviation (wrapped) 
        // it "InletTemperatureDeviation" instead.  Both are accepted.
        {"Redfish.ShutdownAlert.TempDeviation.InletTemperature", "RSCM_ALERT|ShutdownAlert|TempDeviation", "inlet_temperature", FieldType::Number},
        {"Redfish.ShutdownAlert.TempDeviation.Severity",         "RSCM_ALERT|ShutdownAlert|TempDeviation", "severity",          FieldType::String},

        // LiquidPressureDeviation (wrapped)
        {"Redfish.ShutdownAlert.LiquidPressureDeviation.LiquidPressure", "RSCM_ALERT|ShutdownAlert|LiquidPressureDeviation", "liquid_pressure", FieldType::Number},
        {"Redfish.ShutdownAlert.LiquidPressureDeviation.Severity",       "RSCM_ALERT|ShutdownAlert|LiquidPressureDeviation", "severity",        FieldType::String},

        // LeakDetected (wrapped) 
        {"Redfish.ShutdownAlert.LeakDetected.Severity", "RSCM_ALERT|ShutdownAlert|LeakDetected", "severity", FieldType::String},

        // LeakRopeBreak (wrapped)
        {"Redfish.ShutdownAlert.LeakRopeBreak.Severity", "RSCM_ALERT|ShutdownAlert|LeakRopeBreak", "severity", FieldType::String},
    };
    return mappings;
}

} // namespace sonic::dbus_bridge
