///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2026 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// Author: Chinmoy Dey <chinmoy@nexthop.ai>
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

#include <gtest/gtest.h>
#include "leak_sensor_sync.hpp"

#include <optional>
#include <string>

namespace sonic::dbus_bridge::test {

using namespace sonic::dbus_bridge::leak_sync;

namespace {

LeakSensorInfo makeSensor(const std::string& leaking,
                          const std::string& severity = "None",
                          const std::string& status = "Good")
{
    LeakSensorInfo s;
    s.name = "leak_sensor_late";
    s.leaking = leaking;
    s.leakSeverity = severity;
    s.leakSensorStatus = status;
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
// Tracked state: DetectorState plus sensor hardware health
// ---------------------------------------------------------------------------

TEST(TrackedLeakState, CombinesDetectorStateAndHealth)
{
    EXPECT_EQ(trackedLeakState(makeSensor("No")), "OK");
    EXPECT_EQ(trackedLeakState(makeSensor("Yes", "MINOR")), "Warning");
    EXPECT_EQ(trackedLeakState(makeSensor("Yes", "CRITICAL")), "Critical");
    EXPECT_EQ(trackedLeakState(makeSensor("N/A", "None", "Fault")),
              "Unavailable|Fault");
}

// A fault reported alongside an unchanged DetectorState must still count as
// a change, otherwise Functional would never be updated on D-Bus.
TEST(TrackedLeakState, FaultAloneIsAChange)
{
    EXPECT_NE(trackedLeakState(makeSensor("No")),
              trackedLeakState(makeSensor("No", "None", "Fault")));
}

// ---------------------------------------------------------------------------
// Create / Update / None (issue #40: sensors published after startup)
// ---------------------------------------------------------------------------

// The cold boot case: thermalctld writes the row after the bridge's startup
// read, so there is no object yet. It must be created, not dropped.
TEST(DecideLeakSensorAction, UnknownSensorIsCreated)
{
    EXPECT_EQ(decideLeakSensorAction(false, std::nullopt, "OK"),
              LeakSensorAction::Create);
    EXPECT_EQ(decideLeakSensorAction(false, std::nullopt, "Critical"),
              LeakSensorAction::Create);
}

// A stale cache entry never prevents creating a missing object.
TEST(DecideLeakSensorAction, MissingObjectWinsOverCache)
{
    EXPECT_EQ(decideLeakSensorAction(false, std::string("OK"), "OK"),
              LeakSensorAction::Create);
}

// Sensors found at startup have an object but no cache entry: the first
// notification is applied.
TEST(DecideLeakSensorAction, StartupSensorFirstNotificationUpdates)
{
    EXPECT_EQ(decideLeakSensorAction(true, std::nullopt, "OK"),
              LeakSensorAction::Update);
}

TEST(DecideLeakSensorAction, ChangedStateUpdates)
{
    EXPECT_EQ(decideLeakSensorAction(true, std::string("OK"), "Critical"),
              LeakSensorAction::Update);
    EXPECT_EQ(decideLeakSensorAction(true, std::string("OK"), "OK|Fault"),
              LeakSensorAction::Update);
}

// Repeated identical writes, and the one-callback-per-field fan-out of a
// single HSET, must not re-create the object or emit duplicate events.
TEST(DecideLeakSensorAction, UnchangedStateDoesNothing)
{
    EXPECT_EQ(decideLeakSensorAction(true, std::string("Warning"), "Warning"),
              LeakSensorAction::None);
}

// ---------------------------------------------------------------------------
// First state of a sensor created at runtime
// ---------------------------------------------------------------------------

// bmcweb only watches PropertiesChanged, so a detector first seen leaking
// or faulted must be announced; one first seen OK needs no event.
TEST(AnnounceInitialLeakState, OnlyNonOkStatesAreAnnounced)
{
    EXPECT_FALSE(announceInitialLeakState(makeSensor("No")));
    EXPECT_TRUE(announceInitialLeakState(makeSensor("Yes", "MINOR")));
    EXPECT_TRUE(announceInitialLeakState(makeSensor("Yes", "CRITICAL")));
    EXPECT_TRUE(announceInitialLeakState(makeSensor("N/A", "None", "Fault")));
}

} // namespace sonic::dbus_bridge::test
