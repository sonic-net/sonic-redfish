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
// Example:
//   JSON path  "Alarms.InletTempDeviation.InletTemperature"
//   is stored as  HSET RSCM_TELEMETRY|alarms inlet_temp_deviation_temperature <value>
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
    std::string jsonPath;    // Dot-separated path in the input JSON
    std::string redisKey;    // Redis hash key
    std::string redisField;  // Redis hash field name
    FieldType   type;        // Expected value type (for serialisation)
};

// -----------------------------------------------------------------------
// Telemetry mappings  (input: SubmitTelemetry JSON)
//
// To add a new field:
//   1. Add a line here with the JSON path, target Redis key/field, and type.
//   2. That's it -- the receiver walks this table automatically.
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
        {"Alarms.LeakDetected.Severity",      "RSCM_TELEMETRY|alarms", "leak_detected_severity",  FieldType::String},

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
// Same rules as above -- just add/remove lines when alert types change.
// -----------------------------------------------------------------------
inline const std::vector<FieldMapping>& getAlertMappings()
{
    static const std::vector<FieldMapping> mappings = {
        // --- FlowRateDeviation ---
        {"redfish_alert_data.FlowRateDeviation.InletTemperature", "RSCM_ALERT|FlowRateDeviation", "inlet_temperature", FieldType::Number},
        {"redfish_alert_data.FlowRateDeviation.FlowRate",         "RSCM_ALERT|FlowRateDeviation", "flow_rate",         FieldType::Number},
        {"redfish_alert_data.FlowRateDeviation.Severity",         "RSCM_ALERT|FlowRateDeviation", "severity",          FieldType::String},
        {"redfish_alert_data.FlowRateDeviation.RscmPosition",     "RSCM_ALERT|FlowRateDeviation", "rscm_position",     FieldType::Integer},

        // --- LiquidPressureDeviation ---
        {"redfish_alert_data.LiquidPressureDeviation.LiquidPressure", "RSCM_ALERT|LiquidPressureDeviation", "liquid_pressure", FieldType::Number},
        {"redfish_alert_data.LiquidPressureDeviation.Severity",       "RSCM_ALERT|LiquidPressureDeviation", "severity",        FieldType::String},
        {"redfish_alert_data.LiquidPressureDeviation.RscmPosition",   "RSCM_ALERT|LiquidPressureDeviation", "rscm_position",   FieldType::Integer},

        // --- InletTemperatureDeviation ---
        {"redfish_alert_data.InletTemperatureDeviation.InletTemperature", "RSCM_ALERT|InletTemperatureDeviation", "inlet_temperature", FieldType::Number},
        {"redfish_alert_data.InletTemperatureDeviation.Severity",         "RSCM_ALERT|InletTemperatureDeviation", "severity",          FieldType::String},
        {"redfish_alert_data.InletTemperatureDeviation.RscmPosition",     "RSCM_ALERT|InletTemperatureDeviation", "rscm_position",     FieldType::Integer},

        // --- LeakDetected ---
        {"redfish_alert_data.LeakDetected.Severity",     "RSCM_ALERT|LeakDetected", "severity",      FieldType::String},
        {"redfish_alert_data.LeakDetected.RscmPosition", "RSCM_ALERT|LeakDetected", "rscm_position", FieldType::Integer},

        // --- LeakRopeBreak ---
        {"redfish_alert_data.LeakRopeBreak.Severity",     "RSCM_ALERT|LeakRopeBreak", "severity",      FieldType::String},
        {"redfish_alert_data.LeakRopeBreak.RscmPosition", "RSCM_ALERT|LeakRopeBreak", "rscm_position", FieldType::Integer},
    };
    return mappings;
}

} // namespace sonic::dbus_bridge
