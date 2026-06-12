///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2026 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

#include <gtest/gtest.h>
#include "field_mapping.hpp"

#include <regex>
#include <set>
#include <string>
#include <utility>

namespace sonic::dbus_bridge::test {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// All telemetry matches flatten into this single Redis hash key.
constexpr const char* TELEMETRY_REDIS_KEY = "RSCM_TELEMETRY|alarms";

static bool isValidFieldType(FieldType t)
{
    switch (t)
    {
        case FieldType::String:
        case FieldType::Number:
        case FieldType::Integer:
        case FieldType::Boolean:
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Table stability
// ---------------------------------------------------------------------------

TEST(FieldMappingTables, ReturnSameStaticInstanceAcrossCalls)
{
    EXPECT_EQ(&getTelemetryRules(), &getTelemetryRules());
    EXPECT_EQ(&getAlertRules(), &getAlertRules());
}

TEST(FieldMappingTables, BothTablesAreNonEmpty)
{
    EXPECT_FALSE(getTelemetryRules().empty());
    EXPECT_FALSE(getAlertRules().empty());
}

// ---------------------------------------------------------------------------
// Entry-level invariants (apply to every row in every table)
// ---------------------------------------------------------------------------

// Every telemetry rule must carry a non-empty pattern + flattened field, a
// compilable regex, a space/pipe-free Redis field, and a valid value type.
TEST(FieldMappingTables, TelemetryEntriesSatisfyInvariants)
{
    for (const auto& r : getTelemetryRules())
    {
        const std::string ctx = "telemetry[" + r.redisField + "]";
        EXPECT_FALSE(r.pathPattern.empty()) << ctx << ": empty pathPattern";
        EXPECT_FALSE(r.redisField.empty())  << ctx << ": empty redisField";

        // redisField becomes a hash field in TELEMETRY_KEY; spaces/pipes
        // would corrupt the flattened hash.
        EXPECT_EQ(r.redisField.find(' '), std::string::npos)
            << ctx << ": redisField contains a space";
        EXPECT_EQ(r.redisField.find('|'), std::string::npos)
            << ctx << ": redisField contains a '|'";

        // Pattern must be a valid regex (the receiver compiles it icase).
        EXPECT_NO_THROW({ std::regex re(r.pathPattern, std::regex::icase);
                          (void)re; })
            << ctx << ": pathPattern is not a valid regex";

        EXPECT_TRUE(isValidFieldType(r.type)) << ctx << ": invalid FieldType";
    }
}

// Every alert rule must carry a non-empty pattern + canonical name, a
// compilable regex, and a unit iff it is a measurement rule.
TEST(FieldMappingTables, AlertRulesSatisfyInvariants)
{
    for (const auto& r : getAlertRules())
    {
        const std::string ctx = "alert[" + r.alertName + "]";
        EXPECT_FALSE(r.fieldPattern.empty()) << ctx << ": empty fieldPattern";
        EXPECT_FALSE(r.alertName.empty())    << ctx << ": empty alertName";

        // alertName becomes the Redis key suffix; spaces/pipes would corrupt
        // the RACK_MANAGER_ALERT|<name> key.
        EXPECT_EQ(r.alertName.find(' '), std::string::npos)
            << ctx << ": alertName contains a space";
        EXPECT_EQ(r.alertName.find('|'), std::string::npos)
            << ctx << ": alertName contains a '|'";

        // Pattern must be a valid regex (the receiver compiles it icase).
        EXPECT_NO_THROW({ std::regex re(r.fieldPattern, std::regex::icase);
                          (void)re; })
            << ctx << ": fieldPattern is not a valid regex";

        if (r.isMeasurement)
        {
            EXPECT_FALSE(r.unit.empty())
                << ctx << ": measurement rule must define a unit";
        }
        else
        {
            EXPECT_TRUE(r.unit.empty())
                << ctx << ": leak rule must not define a unit";
        }
    }
}

// ---------------------------------------------------------------------------
// Redis key namespacing -- telemetry vs alert tables are kept distinct
// to prevent cross-table collisions in STATE_DB.
// ---------------------------------------------------------------------------

TEST(FieldMappingTables, TelemetryKeyIsCanonical)
{
    EXPECT_STREQ(TELEMETRY_KEY, TELEMETRY_REDIS_KEY);
}

TEST(FieldMappingTables, AlertKeyPrefixIsCanonical)
{
    EXPECT_STREQ(ALERT_KEY_PREFIX, "RACK_MANAGER_ALERT|");
}

// ---------------------------------------------------------------------------
// Duplicate detection: two rows must never target the same (key, field) --
// the second HSET would silently overwrite the first.
// ---------------------------------------------------------------------------

// Two telemetry rules must never target the same flattened field; the second
// HSET would silently overwrite the first within TELEMETRY_KEY.
TEST(FieldMappingTables, NoDuplicateRedisTargetsInTelemetry)
{
    std::set<std::string> seen;
    for (const auto& r : getTelemetryRules())
    {
        EXPECT_TRUE(seen.emplace(r.redisField).second)
            << "telemetry: duplicate redisField '" << r.redisField << "'";
    }
}

// Two rules must never resolve to the same canonical alert name; a later
// HSET would clobber the earlier alert's hash.
TEST(FieldMappingTables, NoDuplicateAlertNames)
{
    std::set<std::string> seen;
    for (const auto& r : getAlertRules())
    {
        EXPECT_TRUE(seen.emplace(r.alertName).second)
            << "duplicate alertName: " << r.alertName;
    }
}

// ---------------------------------------------------------------------------
// Matching semantics -- mirror the receiver's whole-name, case-insensitive
// regex match so the canonical mapping is locked down by tests.
// ---------------------------------------------------------------------------

// Resolve a field/entry name to its canonical alert name ("" if no rule
// matches), restricted to measurement or leak rules per `measurement`.
static std::string resolveAlert(const std::string& name, bool measurement)
{
    for (const auto& r : getAlertRules())
    {
        if (r.isMeasurement != measurement) { continue; }
        std::regex re(r.fieldPattern, std::regex::icase);
        if (std::regex_match(name, re)) { return r.alertName; }
    }
    return "";
}

TEST(AlertMatching, MeasurementFieldsResolveRegardlessOfPrefixOrCase)
{
    EXPECT_EQ(resolveAlert("InletTemperature", true), "Inlet_liquid_temperature");
    EXPECT_EQ(resolveAlert("SmartItInletTemperature", true),
              "Inlet_liquid_temperature");
    EXPECT_EQ(resolveAlert("inlettemperature", true),
              "Inlet_liquid_temperature");
    EXPECT_EQ(resolveAlert("FlowRate", true), "Inlet_liquid_flow_rate");
    EXPECT_EQ(resolveAlert("LiquidPressure", true), "Inlet_liquid_pressure");
}

TEST(AlertMatching, LeakEntriesResolveByName)
{
    EXPECT_EQ(resolveAlert("LeakDetected", false), "leak_detected");
    EXPECT_EQ(resolveAlert("SmartItLeakDetected", false), "leak_detected");
    EXPECT_EQ(resolveAlert("LeakRopeBreak", false), "leak_rope_break");
    EXPECT_EQ(resolveAlert("SmartItLeakRopeBreak", false), "leak_rope_break");
}

TEST(AlertMatching, WrapperNamesDoNotMatchMeasurements)
{
    // Wrapper objects (e.g. "FlowRateDeviation") must NOT be treated as a
    // leaf measurement; only their inner numeric leaves are.
    EXPECT_EQ(resolveAlert("FlowRateDeviation", true), "");
    EXPECT_EQ(resolveAlert("InletTemperatureDeviation", true), "");
    EXPECT_EQ(resolveAlert("ShutdownAlert", false), "");
}

// ---------------------------------------------------------------------------
// Telemetry matching -- mirror the receiver's whole-name, case-insensitive
// match of a leaf's trailing dotted path against the telemetry rules.
// ---------------------------------------------------------------------------

// Resolve a leaf's trailing dotted path to its flattened field ("" if none).
static std::string resolveTelemetry(const std::string& path)
{
    for (const auto& r : getTelemetryRules())
    {
        std::regex re(r.pathPattern, std::regex::icase);
        if (std::regex_match(path, re)) { return r.redisField; }
    }
    return "";
}

TEST(TelemetryMatching, FlatLeavesResolveByName)
{
    EXPECT_EQ(resolveTelemetry("EnergyValveActive"), "energy_valve_active");
    EXPECT_EQ(resolveTelemetry("TemperatureSensorActive"),
              "temperature_sensor_active");
    EXPECT_EQ(resolveTelemetry("GlycolConcentration"), "glycol_concentration");
    EXPECT_EQ(resolveTelemetry("RscmPosition"), "rscm_position");
}

TEST(TelemetryMatching, NestedLeavesResolveRegardlessOfPrefix)
{
    EXPECT_EQ(resolveTelemetry("InletTempDeviation.InletTemperature"),
              "inlet_temp_deviation_temperature");
    // A leading prefix (e.g. the envelope name) must not break the match.
    EXPECT_EQ(resolveTelemetry("Alarms.InletTempDeviation.InletTemperature"),
              "inlet_temp_deviation_temperature");
    EXPECT_EQ(resolveTelemetry("LeakDetected.Severity"),
              "leak_detected_severity");
    EXPECT_EQ(resolveTelemetry("FlowRateDeviation.FlowRate"),
              "flow_rate_deviation_flow_rate");
}

TEST(TelemetryMatching, UnknownLeavesDoNotMatch)
{
    EXPECT_EQ(resolveTelemetry("Unknown.Leaf"), "");
    EXPECT_EQ(resolveTelemetry("InletTempDeviation"), "");
}

} // namespace sonic::dbus_bridge::test
