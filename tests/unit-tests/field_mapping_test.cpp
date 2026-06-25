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

// Resolve a field/entry name to its canonical sensor name ("" if no rule
// matches), restricted to a given SensorKind.  Mirrors the receiver's
// whole-name, case-insensitive regex match.
static std::string resolveSensor(const std::string& name, SensorKind kind)
{
    for (const auto& r : getSensorRules())
    {
        if (r.kind != kind) { continue; }
        std::regex re(r.fieldPattern, std::regex::icase);
        if (std::regex_match(name, re)) { return r.sensorName; }
    }
    return "";
}

// ---------------------------------------------------------------------------
// Table stability / non-emptiness
// ---------------------------------------------------------------------------

TEST(SensorRuleTable, ReturnsSameStaticInstanceAcrossCalls)
{
    EXPECT_EQ(&getSensorRules(), &getSensorRules());
}

TEST(SensorRuleTable, IsNonEmpty)
{
    EXPECT_FALSE(getSensorRules().empty());
}

// ---------------------------------------------------------------------------
// Key prefixes match the platform DB schema.
// ---------------------------------------------------------------------------

TEST(SensorRuleTable, KeyPrefixesAreCanonical)
{
    EXPECT_STREQ(DATA_KEY_PREFIX, "RACK_MANAGER_DATA|");
    EXPECT_STREQ(ALERT_KEY_PREFIX, "RACK_MANAGER_ALERT|");
}

// ---------------------------------------------------------------------------
// Entry-level invariants (apply to every row).
// ---------------------------------------------------------------------------

// Every rule must carry a non-empty pattern + canonical name, a compilable
// regex, a space/pipe-free sensor name, and unit/valueField iff measurement.
TEST(SensorRuleTable, EntriesSatisfyInvariants)
{
    for (const auto& r : getSensorRules())
    {
        const std::string ctx = "sensor[" + r.sensorName + "]";
        EXPECT_FALSE(r.fieldPattern.empty()) << ctx << ": empty fieldPattern";
        EXPECT_FALSE(r.sensorName.empty())   << ctx << ": empty sensorName";

        // sensorName becomes the Redis key suffix; spaces/pipes would corrupt
        // the RACK_MANAGER_{DATA,ALERT}|<name> key.
        EXPECT_EQ(r.sensorName.find(' '), std::string::npos)
            << ctx << ": sensorName contains a space";
        EXPECT_EQ(r.sensorName.find('|'), std::string::npos)
            << ctx << ": sensorName contains a '|'";

        // Pattern must be a valid regex (the receiver compiles it icase).
        EXPECT_NO_THROW({ std::regex re(r.fieldPattern, std::regex::icase);
                          (void)re; })
            << ctx << ": fieldPattern is not a valid regex";

        if (r.kind == SensorKind::Measurement)
        {
            EXPECT_FALSE(r.unit.empty())
                << ctx << ": measurement rule must define a unit";
            EXPECT_FALSE(r.valueField.empty())
                << ctx << ": measurement rule must define a value field";
        }
        else
        {
            EXPECT_TRUE(r.unit.empty())
                << ctx << ": leak rule must not define a unit";
            EXPECT_TRUE(r.valueField.empty())
                << ctx << ": leak rule must not define a value field";
        }
    }
}

// ---------------------------------------------------------------------------
// Duplicate detection: two rows must never resolve to the same canonical
// sensor name; a later HSET would clobber the earlier sensor's hash.
// ---------------------------------------------------------------------------

TEST(SensorRuleTable, NoDuplicateSensorNames)
{
    std::set<std::string> seen;
    for (const auto& r : getSensorRules())
    {
        EXPECT_TRUE(seen.emplace(r.sensorName).second)
            << "duplicate sensorName: " << r.sensorName;
    }
}

// ---------------------------------------------------------------------------
// Matching semantics -- mirror the receiver's whole-name, case-insensitive
// regex match so the canonical mapping is locked down by tests.
// ---------------------------------------------------------------------------

TEST(SensorMatching, MeasurementFieldsResolveRegardlessOfPrefixOrCase)
{
    EXPECT_EQ(resolveSensor("InletTemperature", SensorKind::Measurement),
              "Inlet_liquid_temperature");
    EXPECT_EQ(resolveSensor("SmartItInletTemperature", SensorKind::Measurement),
              "Inlet_liquid_temperature");
    EXPECT_EQ(resolveSensor("inlettemperature", SensorKind::Measurement),
              "Inlet_liquid_temperature");
    EXPECT_EQ(resolveSensor("FlowRate", SensorKind::Measurement),
              "Inlet_liquid_flow_rate");
    EXPECT_EQ(resolveSensor("LiquidPressure", SensorKind::Measurement),
              "Inlet_liquid_pressure");
}

TEST(SensorMatching, LeakEntriesCollapseToRackLevelLeak)
{
    EXPECT_EQ(resolveSensor("LeakDetected", SensorKind::Leak),
              "Rack_level_leak");
    EXPECT_EQ(resolveSensor("LeakRopeBreak", SensorKind::Leak),
              "Rack_level_leak");
    EXPECT_EQ(resolveSensor("SmartItLeakDetected", SensorKind::Leak),
              "Rack_level_leak");
}

TEST(SensorMatching, WrapperNamesDoNotMatchMeasurements)
{
    // Wrapper objects (e.g. "FlowRateDeviation") must NOT be treated as a
    // leaf measurement; only their inner numeric leaves are.
    EXPECT_EQ(resolveSensor("FlowRateDeviation", SensorKind::Measurement), "");
    EXPECT_EQ(resolveSensor("InletTempDeviation", SensorKind::Measurement), "");
    EXPECT_EQ(resolveSensor("ShutdownAlert", SensorKind::Leak), "");
}

// Measurement and leak rules occupy disjoint name spaces for the canonical
// inputs (a measurement field never resolves as a leak and vice versa).
TEST(SensorMatching, MeasurementAndLeakSpacesAreDisjoint)
{
    EXPECT_EQ(resolveSensor("InletTemperature", SensorKind::Leak), "");
    EXPECT_EQ(resolveSensor("FlowRate", SensorKind::Leak), "");
    EXPECT_EQ(resolveSensor("LeakDetected", SensorKind::Measurement), "");
}

} // namespace sonic::dbus_bridge::test
