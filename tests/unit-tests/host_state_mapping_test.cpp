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
#include "host_state_mapping.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <string_view>

namespace sonic::dbus_bridge::test {

using namespace sonic::dbus_bridge::host_state;

// ---------------------------------------------------------------------------
// HOST_STATE|switch-host -> xyz.openbmc_project.State.Host.HostState
// ---------------------------------------------------------------------------

struct HostStateCase
{
    std::string_view devicePowerState;
    std::string_view deviceStatus;
    std::string_view expected;
};

// Transitional device_power_state values map directly. device_status is
// ignored because bmcctld does not update it until the transition settles.
TEST(MapHostState, TransitionalStatesIgnoreDeviceStatus)
{
    constexpr HostStateCase cases[] = {
        {"POWERING_ON",            "OFFLINE", HOST_STATE_TRANSITIONING_TO_RUNNING},
        {"POWERING_ON",            "ONLINE",  HOST_STATE_TRANSITIONING_TO_RUNNING},
        {"POWERING_ON",            "",        HOST_STATE_TRANSITIONING_TO_RUNNING},
        {"POWERING_OFF",           "ONLINE",  HOST_STATE_TRANSITIONING_TO_OFF},
        {"POWERING_OFF",           "OFFLINE", HOST_STATE_TRANSITIONING_TO_OFF},
        {"GRACEFUL_SHUTTING_DOWN", "ONLINE",  HOST_STATE_TRANSITIONING_TO_OFF},
        {"POWER_CYCLING",          "ONLINE",  HOST_STATE_TRANSITIONING_TO_RUNNING},
        {"POWER_CYCLING",          "OFFLINE", HOST_STATE_TRANSITIONING_TO_RUNNING},
    };
    for (const auto& c : cases)
    {
        auto got = mapHostState(c.devicePowerState, c.deviceStatus);
        ASSERT_TRUE(got.has_value()) << c.devicePowerState << "/" << c.deviceStatus;
        EXPECT_EQ(*got, c.expected) << c.devicePowerState << "/" << c.deviceStatus;
    }
}

// Stable device_power_state values agree with device_status in the normal
// case.
TEST(MapHostState, StableStatesConsistentWithDeviceStatus)
{
    EXPECT_EQ(*mapHostState("POWERED_ON",        "ONLINE"),  HOST_STATE_RUNNING);
    EXPECT_EQ(*mapHostState("POWERED_OFF",       "OFFLINE"), HOST_STATE_OFF);
    EXPECT_EQ(*mapHostState("GRACEFUL_SHUTDOWN", "OFFLINE"), HOST_STATE_OFF);
    EXPECT_EQ(*mapHostState("POWER_CYCLE",       "ONLINE"),  HOST_STATE_RUNNING);
}

// For stable states, device_status is the source of truth when it disagrees
// with device_power_state (bmcctld writes the target power state even if the
// platform did not confirm it).
TEST(MapHostState, StableStatesDeferToDeviceStatusOnConflict)
{
    EXPECT_EQ(*mapHostState("POWERED_ON",        "OFFLINE"), HOST_STATE_OFF);
    EXPECT_EQ(*mapHostState("POWERED_OFF",       "ONLINE"),  HOST_STATE_RUNNING);
    EXPECT_EQ(*mapHostState("GRACEFUL_SHUTDOWN", "ONLINE"),  HOST_STATE_RUNNING);
    EXPECT_EQ(*mapHostState("POWER_CYCLE",       "OFFLINE"), HOST_STATE_OFF);
}

// Stable state falls back to the table value when device_status is missing
// or unknown.
TEST(MapHostState, StableStatesFallBackWhenDeviceStatusUnknown)
{
    EXPECT_EQ(*mapHostState("POWERED_ON",        ""),        HOST_STATE_RUNNING);
    EXPECT_EQ(*mapHostState("POWERED_OFF",       ""),        HOST_STATE_OFF);
    EXPECT_EQ(*mapHostState("GRACEFUL_SHUTDOWN", "UNKNOWN"), HOST_STATE_OFF);
    EXPECT_EQ(*mapHostState("POWER_CYCLE",       "bogus"),   HOST_STATE_RUNNING);
}

// Unknown device_power_state: resolve from device_status alone.
TEST(MapHostState, UnknownPowerStateResolvesFromDeviceStatus)
{
    EXPECT_EQ(*mapHostState("",         "ONLINE"),  HOST_STATE_RUNNING);
    EXPECT_EQ(*mapHostState("",         "OFFLINE"), HOST_STATE_OFF);
    EXPECT_EQ(*mapHostState("WHATEVER", "ONLINE"),  HOST_STATE_RUNNING);
    EXPECT_EQ(*mapHostState("WHATEVER", "OFFLINE"), HOST_STATE_OFF);
}

// Nothing recognised -> nullopt so the caller keeps its current state.
TEST(MapHostState, UnrecognisedInputsYieldNullopt)
{
    EXPECT_FALSE(mapHostState("", "").has_value());
    EXPECT_FALSE(mapHostState("WHATEVER", "").has_value());
    EXPECT_FALSE(mapHostState("WHATEVER", "UNKNOWN").has_value());
    // Case-sensitive: lower-case variants are not part of the contract.
    EXPECT_FALSE(mapHostState("powered_on", "online").has_value());
}

TEST(MapHostState, DeviceStatusHelper)
{
    EXPECT_EQ(*hostStateFromDeviceStatus("ONLINE"),  HOST_STATE_RUNNING);
    EXPECT_EQ(*hostStateFromDeviceStatus("OFFLINE"), HOST_STATE_OFF);
    EXPECT_FALSE(hostStateFromDeviceStatus("").has_value());
    EXPECT_FALSE(hostStateFromDeviceStatus("online").has_value());
}

// Every table entry is reachable and maps to a well-formed HostState enum.
TEST(DevicePowerStateTable, AllRulesProduceValidHostState)
{
    const std::set<std::string_view> valid = {
        HOST_STATE_OFF, HOST_STATE_RUNNING,
        HOST_STATE_TRANSITIONING_TO_RUNNING, HOST_STATE_TRANSITIONING_TO_OFF};
    std::set<std::string_view> seen;
    for (const auto& rule : DEVICE_POWER_STATE_RULES)
    {
        EXPECT_FALSE(rule.devicePowerState.empty());
        EXPECT_TRUE(valid.count(rule.hostState)) << rule.devicePowerState;
        EXPECT_TRUE(seen.insert(rule.devicePowerState).second)
            << "duplicate rule: " << rule.devicePowerState;
        // With no device_status every rule must resolve to its own hostState.
        EXPECT_EQ(*mapHostState(rule.devicePowerState, ""), rule.hostState);
    }
}

// Chassis.PowerState derived from HostState for the chassis0 object.
TEST(HostStateToChassisPowerState, Mapping)
{
    EXPECT_EQ(hostStateToChassisPowerState(HOST_STATE_RUNNING),
              CHASSIS_POWER_STATE_ON);
    EXPECT_EQ(hostStateToChassisPowerState(HOST_STATE_OFF),
              CHASSIS_POWER_STATE_OFF);
    EXPECT_EQ(hostStateToChassisPowerState(HOST_STATE_TRANSITIONING_TO_RUNNING),
              CHASSIS_POWER_STATE_TRANSITIONING_TO_ON);
    EXPECT_EQ(hostStateToChassisPowerState(HOST_STATE_TRANSITIONING_TO_OFF),
              CHASSIS_POWER_STATE_TRANSITIONING_TO_OFF);
    // Unknown / Quiesced-style inputs are treated as Off (safe default).
    EXPECT_EQ(hostStateToChassisPowerState(""), CHASSIS_POWER_STATE_OFF);
    EXPECT_EQ(hostStateToChassisPowerState(
                  "xyz.openbmc_project.State.Host.HostState.Quiesced"),
              CHASSIS_POWER_STATE_OFF);
}

// ---------------------------------------------------------------------------
// Startup: HOST_STATE|switch-host absent or unreadable -> Unknown, never a guess
// ---------------------------------------------------------------------------

// The dedicated unknown values are outside the OpenBMC enums on purpose:
// bmcweb must recognise them explicitly and must not fold them into Off.
TEST(UnknownState, ValuesAreDistinctFromOpenBmcEnums)
{
    EXPECT_TRUE(HOST_STATE_UNKNOWN.starts_with(
        "xyz.openbmc_project.State.Host.HostState."));
    EXPECT_TRUE(CHASSIS_POWER_STATE_UNKNOWN.starts_with(
        "xyz.openbmc_project.State.Chassis.PowerState."));
    const std::set<std::string_view> hostEnums = {
        HOST_STATE_OFF, HOST_STATE_RUNNING,
        HOST_STATE_TRANSITIONING_TO_RUNNING, HOST_STATE_TRANSITIONING_TO_OFF};
    EXPECT_FALSE(hostEnums.count(HOST_STATE_UNKNOWN));
    const std::set<std::string_view> chassisEnums = {
        CHASSIS_POWER_STATE_ON, CHASSIS_POWER_STATE_OFF,
        CHASSIS_POWER_STATE_TRANSITIONING_TO_ON,
        CHASSIS_POWER_STATE_TRANSITIONING_TO_OFF};
    EXPECT_FALSE(chassisEnums.count(CHASSIS_POWER_STATE_UNKNOWN));
}

// No HOST_STATE|switch-host row yet (bmcctld has not published): Unknown,
// whatever the caller passes for the fields.
TEST(InitialHostState, AbsentRowIsUnknown)
{
    EXPECT_EQ(initialHostState(false, "", ""), HOST_STATE_UNKNOWN);
    EXPECT_EQ(initialHostState(false, "POWERED_ON", "ONLINE"), HOST_STATE_UNKNOWN);
}

// A present, recognised row seeds the real state through the same table
// that runtime updates use.
TEST(InitialHostState, RecognisedRowMapsLikeRuntimeUpdates)
{
    EXPECT_EQ(initialHostState(true, "POWERED_ON", "ONLINE"), HOST_STATE_RUNNING);
    EXPECT_EQ(initialHostState(true, "POWERED_OFF", "OFFLINE"), HOST_STATE_OFF);
    EXPECT_EQ(initialHostState(true, "POWERING_OFF", "ONLINE"),
              HOST_STATE_TRANSITIONING_TO_OFF);
    EXPECT_EQ(initialHostState(true, "POWERED_ON", "OFFLINE"), HOST_STATE_OFF);
    EXPECT_EQ(initialHostState(true, "", "ONLINE"), HOST_STATE_RUNNING);
}

// A present row that maps to nothing (empty or unrecognised fields, e.g. a
// row bmcctld wrote with device_status N/A before it could read the module)
// is also Unknown rather than a guess.
TEST(InitialHostState, UnrecognisedRowIsUnknown)
{
    EXPECT_EQ(initialHostState(true, "", ""), HOST_STATE_UNKNOWN);
    EXPECT_EQ(initialHostState(true, "WHATEVER", "N/A"), HOST_STATE_UNKNOWN);
    EXPECT_EQ(initialHostState(true, "powered_on", "online"), HOST_STATE_UNKNOWN);
}

// Unknown host state must not be reported as a powered-off chassis.
TEST(HostStateToChassisPowerState, UnknownStaysUnknown)
{
    EXPECT_EQ(hostStateToChassisPowerState(HOST_STATE_UNKNOWN),
              CHASSIS_POWER_STATE_UNKNOWN);
}

TEST(UnknownState, Predicate)
{
    EXPECT_TRUE(isUnknownHostState(HOST_STATE_UNKNOWN));
    EXPECT_FALSE(isUnknownHostState(HOST_STATE_OFF));
    EXPECT_FALSE(isUnknownHostState(HOST_STATE_RUNNING));
    EXPECT_FALSE(isUnknownHostState(""));
}

// ---------------------------------------------------------------------------
// Redfish ResetType -> D-Bus transition -> RACK_MANAGER_COMMAND command
// ---------------------------------------------------------------------------

struct ResetTypeCase
{
    std::string_view resetType;
    std::string_view transition;
    std::string_view command;
};

// The four supported ResetTypes, end to end.
TEST(ResetTypeMapping, SupportedResetTypesEndToEnd)
{
    constexpr ResetTypeCase cases[] = {
        {"On",               HOST_TRANSITION_ON,     CMD_POWER_ON},
        {"ForceOff",         CHASSIS_TRANSITION_OFF, CMD_POWER_OFF},
        {"GracefulShutdown", HOST_TRANSITION_OFF,    CMD_GRACEFUL_SHUT},
        {"PowerCycle",       HOST_TRANSITION_REBOOT, CMD_POWER_CYCLE},
    };
    for (const auto& c : cases)
    {
        auto transition = resetTypeToTransition(c.resetType);
        ASSERT_TRUE(transition.has_value()) << c.resetType;
        EXPECT_EQ(*transition, c.transition) << c.resetType;

        auto command = transitionToCommand(*transition);
        ASSERT_TRUE(command.has_value()) << c.resetType;
        EXPECT_EQ(*command, c.command) << c.resetType;
    }
}

// ResetTypes the Switch-Host cannot perform must not resolve to a transition
// (bmcweb patch returns ActionParameterNotSupported for these).
TEST(ResetTypeMapping, UnsupportedResetTypesAreRejected)
{
    constexpr std::string_view unsupported[] = {
        "ForceOn", "ForceRestart", "GracefulRestart", "Nmi",
        "PushPowerButton", "Pause", "Resume", "Suspend", "FullPowerCycle",
        "", "on", "poweroff",
    };
    for (auto rt : unsupported)
    {
        EXPECT_FALSE(resetTypeToTransition(rt).has_value()) << rt;
    }
}

// SUPPORTED_RESET_TYPES and the TRANSITION_RULES resetType column agree.
TEST(ResetTypeMapping, SupportedListMatchesRuleTable)
{
    std::set<std::string_view> fromRules;
    for (const auto& rule : TRANSITION_RULES)
    {
        if (!rule.resetType.empty())
        {
            EXPECT_TRUE(fromRules.insert(rule.resetType).second)
                << "duplicate ResetType: " << rule.resetType;
        }
    }
    std::set<std::string_view> fromList(SUPPORTED_RESET_TYPES.begin(),
                                        SUPPORTED_RESET_TYPES.end());
    EXPECT_EQ(fromRules, fromList);
    for (auto rt : SUPPORTED_RESET_TYPES)
    {
        EXPECT_TRUE(resetTypeToTransition(rt).has_value()) << rt;
    }
}

// Transitions accepted on D-Bus without a Redfish ResetType still map to a
// command (defensive: direct D-Bus callers).
TEST(TransitionToCommand, DbusOnlyTransitions)
{
    EXPECT_EQ(*transitionToCommand(HOST_TRANSITION_FORCE_WARM_REBOOT), CMD_POWER_CYCLE);
    EXPECT_EQ(*transitionToCommand(HOST_TRANSITION_POWER_CYCLE),       CMD_POWER_CYCLE);
    EXPECT_EQ(*transitionToCommand(CHASSIS_TRANSITION_ON),             CMD_POWER_ON);
}

TEST(TransitionToCommand, UnknownTransitionsYieldNullopt)
{
    EXPECT_FALSE(transitionToCommand("").has_value());
    EXPECT_FALSE(transitionToCommand("On").has_value());
    EXPECT_FALSE(transitionToCommand(
                     "xyz.openbmc_project.State.Host.Transition.GracefulWarmReboot")
                     .has_value());
    EXPECT_FALSE(transitionToCommand(
                     "xyz.openbmc_project.State.Host.Transition.on")
                     .has_value());
}

// Every rule's command is in the bmcctld vocabulary and the transition
// carries the correct D-Bus prefix.
TEST(TransitionRuleTable, AllRulesWellFormed)
{
    const std::set<std::string_view> commands = {
        CMD_POWER_ON, CMD_POWER_OFF, CMD_GRACEFUL_SHUT, CMD_POWER_CYCLE};
    std::set<std::string_view> seen;
    for (const auto& rule : TRANSITION_RULES)
    {
        EXPECT_TRUE(commands.count(rule.command)) << rule.transition;
        EXPECT_TRUE(seen.insert(rule.transition).second)
            << "duplicate transition: " << rule.transition;
        EXPECT_NE(isHostTransition(rule.transition),
                  isChassisTransition(rule.transition))
            << rule.transition;
        EXPECT_EQ(*transitionToCommand(rule.transition), rule.command);
    }
}

TEST(TransitionClassification, HostVsChassis)
{
    EXPECT_TRUE(isHostTransition(HOST_TRANSITION_ON));
    EXPECT_TRUE(isHostTransition(HOST_TRANSITION_OFF));
    EXPECT_TRUE(isHostTransition(HOST_TRANSITION_REBOOT));
    EXPECT_FALSE(isHostTransition(CHASSIS_TRANSITION_OFF));
    EXPECT_TRUE(isChassisTransition(CHASSIS_TRANSITION_OFF));
    EXPECT_TRUE(isChassisTransition(CHASSIS_TRANSITION_ON));
    EXPECT_FALSE(isChassisTransition(HOST_TRANSITION_ON));
    // Right prefix but not a supported transition.
    EXPECT_FALSE(isHostTransition(
        "xyz.openbmc_project.State.Host.Transition.GracefulWarmReboot"));
    EXPECT_FALSE(isChassisTransition(
        "xyz.openbmc_project.State.Chassis.Transition.PowerCycle"));
}

// ---------------------------------------------------------------------------
// RACK_MANAGER_COMMAND status lifecycle
// ---------------------------------------------------------------------------

TEST(CommandStatus, TerminalStates)
{
    EXPECT_TRUE(isTerminalCommandStatus(CMD_STATUS_DONE));
    EXPECT_TRUE(isTerminalCommandStatus(CMD_STATUS_FAILED));
    EXPECT_FALSE(isTerminalCommandStatus(CMD_STATUS_PENDING));
    EXPECT_FALSE(isTerminalCommandStatus(CMD_STATUS_IN_PROGRESS));
    EXPECT_FALSE(isTerminalCommandStatus(""));
    EXPECT_FALSE(isTerminalCommandStatus("done"));
}

// Contract constants must match the bmcctld schema exactly.
TEST(SchemaConstants, MatchBmcctldContract)
{
    EXPECT_EQ(KEY_HOST_STATE, "HOST_STATE|switch-host");
    EXPECT_EQ(TABLE_RACK_MANAGER_COMMAND, "RACK_MANAGER_COMMAND");
    EXPECT_EQ(FIELD_DEVICE_POWER_STATE, "device_power_state");
    EXPECT_EQ(FIELD_DEVICE_STATUS, "device_status");
    EXPECT_EQ(FIELD_COMMAND, "command");
    EXPECT_EQ(FIELD_STATUS, "status");
    EXPECT_EQ(FIELD_RESULT, "result");
}

} // namespace sonic::dbus_bridge::test
