///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2026 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// Author: Chinmoy Dey <chinmoy@nexthop.ai>
// License file: sonic-redfish/LICENSE
///////////////////////////////////////
//
// Header-only mapping between the bmcctld STATE_DB contract
// (HOST_STATE|switch-host, RACK_MANAGER_COMMAND|<id>) and the OpenBMC
// xyz.openbmc_project.State.{Host,Chassis} D-Bus vocabulary consumed by
// bmcweb. No external dependencies so it can be unit tested standalone.

#pragma once

#include <array>
#include <optional>
#include <string_view>

namespace sonic::dbus_bridge::host_state
{

// xyz.openbmc_project.State.Host.HostState
inline constexpr std::string_view HOST_STATE_OFF =
    "xyz.openbmc_project.State.Host.HostState.Off";
inline constexpr std::string_view HOST_STATE_RUNNING =
    "xyz.openbmc_project.State.Host.HostState.Running";
inline constexpr std::string_view HOST_STATE_TRANSITIONING_TO_RUNNING =
    "xyz.openbmc_project.State.Host.HostState.TransitioningToRunning";
inline constexpr std::string_view HOST_STATE_TRANSITIONING_TO_OFF =
    "xyz.openbmc_project.State.Host.HostState.TransitioningToOff";

// xyz.openbmc_project.State.Host.Transition
inline constexpr std::string_view HOST_TRANSITION_ON =
    "xyz.openbmc_project.State.Host.Transition.On";
inline constexpr std::string_view HOST_TRANSITION_OFF =
    "xyz.openbmc_project.State.Host.Transition.Off";
inline constexpr std::string_view HOST_TRANSITION_REBOOT =
    "xyz.openbmc_project.State.Host.Transition.Reboot";
inline constexpr std::string_view HOST_TRANSITION_FORCE_WARM_REBOOT =
    "xyz.openbmc_project.State.Host.Transition.ForceWarmReboot";
inline constexpr std::string_view HOST_TRANSITION_POWER_CYCLE =
    "xyz.openbmc_project.State.Host.Transition.PowerCycle";

// xyz.openbmc_project.State.Chassis.Transition / PowerState
inline constexpr std::string_view CHASSIS_TRANSITION_ON =
    "xyz.openbmc_project.State.Chassis.Transition.On";
inline constexpr std::string_view CHASSIS_TRANSITION_OFF =
    "xyz.openbmc_project.State.Chassis.Transition.Off";
inline constexpr std::string_view CHASSIS_POWER_STATE_ON =
    "xyz.openbmc_project.State.Chassis.PowerState.On";
inline constexpr std::string_view CHASSIS_POWER_STATE_OFF =
    "xyz.openbmc_project.State.Chassis.PowerState.Off";
inline constexpr std::string_view CHASSIS_POWER_STATE_TRANSITIONING_TO_ON =
    "xyz.openbmc_project.State.Chassis.PowerState.TransitioningToOn";
inline constexpr std::string_view CHASSIS_POWER_STATE_TRANSITIONING_TO_OFF =
    "xyz.openbmc_project.State.Chassis.PowerState.TransitioningToOff";

// Reported until bmcctld publishes HOST_STATE|switch-host. These are not
// OpenBMC enum members: stock bmcweb folds any value it does not recognise
// into Off, so the SONiC bmcweb patch handles them explicitly (PowerState
// omitted, Status.State UnavailableOffline) and the event monitor ignores
// them.
inline constexpr std::string_view HOST_STATE_UNKNOWN =
    "xyz.openbmc_project.State.Host.HostState.Unknown";
inline constexpr std::string_view CHASSIS_POWER_STATE_UNKNOWN =
    "xyz.openbmc_project.State.Chassis.PowerState.Unknown";

// STATE_DB keys / fields written by bmcctld
inline constexpr std::string_view KEY_HOST_STATE = "HOST_STATE|switch-host";
inline constexpr std::string_view TABLE_RACK_MANAGER_COMMAND = "RACK_MANAGER_COMMAND";
inline constexpr std::string_view FIELD_DEVICE_POWER_STATE = "device_power_state";
inline constexpr std::string_view FIELD_DEVICE_STATUS = "device_status";
inline constexpr std::string_view FIELD_COMMAND = "command";
inline constexpr std::string_view FIELD_STATUS = "status";
inline constexpr std::string_view FIELD_RESULT = "result";

// HOST_STATE|switch-host device_status
inline constexpr std::string_view DEVICE_STATUS_ONLINE = "ONLINE";
inline constexpr std::string_view DEVICE_STATUS_OFFLINE = "OFFLINE";

// RACK_MANAGER_COMMAND command vocabulary
inline constexpr std::string_view CMD_POWER_ON = "POWER_ON";
inline constexpr std::string_view CMD_POWER_OFF = "POWER_OFF";
inline constexpr std::string_view CMD_GRACEFUL_SHUT = "GRACEFUL_SHUT";
inline constexpr std::string_view CMD_POWER_CYCLE = "POWER_CYCLE";

// RACK_MANAGER_COMMAND status vocabulary
inline constexpr std::string_view CMD_STATUS_PENDING = "PENDING";
inline constexpr std::string_view CMD_STATUS_IN_PROGRESS = "IN_PROGRESS";
inline constexpr std::string_view CMD_STATUS_DONE = "DONE";
inline constexpr std::string_view CMD_STATUS_FAILED = "FAILED";

/**
 * @brief HOST_STATE|switch-host device_power_state -> HostState rule.
 *
 * Transitional states map directly to a HostState. Stable states are
 * resolved from device_status (ONLINE -> Running, OFFLINE -> Off) because
 * bmcctld writes the stable device_power_state even when the platform did
 * not confirm the transition. hostState is the fallback when device_status
 * is unavailable.
 */
struct DevicePowerStateRule
{
    std::string_view devicePowerState;
    bool transitional;
    std::string_view hostState;
};

inline constexpr std::array<DevicePowerStateRule, 8> DEVICE_POWER_STATE_RULES = {{
    {"POWERING_ON",            true,  HOST_STATE_TRANSITIONING_TO_RUNNING},
    {"POWERING_OFF",           true,  HOST_STATE_TRANSITIONING_TO_OFF},
    {"GRACEFUL_SHUTTING_DOWN", true,  HOST_STATE_TRANSITIONING_TO_OFF},
    {"POWER_CYCLING",          true,  HOST_STATE_TRANSITIONING_TO_RUNNING},
    {"POWERED_ON",             false, HOST_STATE_RUNNING},
    {"POWERED_OFF",            false, HOST_STATE_OFF},
    {"GRACEFUL_SHUTDOWN",      false, HOST_STATE_OFF},
    {"POWER_CYCLE",            false, HOST_STATE_RUNNING},
}};

/**
 * @brief D-Bus transition -> RACK_MANAGER_COMMAND command rule.
 *
 * resetType documents the Redfish ComputerSystem.Reset ResetType that bmcweb
 * translates into the given D-Bus transition (empty when the transition is
 * accepted on D-Bus but not exposed as a Redfish ResetType).
 */
struct TransitionRule
{
    std::string_view resetType;
    std::string_view transition;
    std::string_view command;
};

inline constexpr std::array<TransitionRule, 7> TRANSITION_RULES = {{
    {"On",               HOST_TRANSITION_ON,                CMD_POWER_ON},
    {"ForceOff",         CHASSIS_TRANSITION_OFF,            CMD_POWER_OFF},
    {"GracefulShutdown", HOST_TRANSITION_OFF,               CMD_GRACEFUL_SHUT},
    {"PowerCycle",       HOST_TRANSITION_REBOOT,            CMD_POWER_CYCLE},
    {"",                 HOST_TRANSITION_FORCE_WARM_REBOOT, CMD_POWER_CYCLE},
    {"",                 HOST_TRANSITION_POWER_CYCLE,       CMD_POWER_CYCLE},
    {"",                 CHASSIS_TRANSITION_ON,             CMD_POWER_ON},
}};

/// Redfish ResetTypes accepted by ComputerSystem.Reset on this platform.
inline constexpr std::array<std::string_view, 4> SUPPORTED_RESET_TYPES = {
    "On", "ForceOff", "GracefulShutdown", "PowerCycle"};

/**
 * @brief Resolve device_status alone to a stable HostState.
 * @return Running for ONLINE, Off for OFFLINE, nullopt otherwise.
 */
inline constexpr std::optional<std::string_view>
    hostStateFromDeviceStatus(std::string_view deviceStatus)
{
    if (deviceStatus == DEVICE_STATUS_ONLINE)
    {
        return HOST_STATE_RUNNING;
    }
    if (deviceStatus == DEVICE_STATUS_OFFLINE)
    {
        return HOST_STATE_OFF;
    }
    return std::nullopt;
}

/**
 * @brief Map HOST_STATE|switch-host fields to a D-Bus HostState.
 *
 * @param devicePowerState device_power_state field (may be empty)
 * @param deviceStatus     device_status field (may be empty)
 * @return HostState enum string, or nullopt if neither field is
 *         recognised (caller should keep its current state).
 */
inline constexpr std::optional<std::string_view>
    mapHostState(std::string_view devicePowerState, std::string_view deviceStatus)
{
    for (const auto& rule : DEVICE_POWER_STATE_RULES)
    {
        if (rule.devicePowerState != devicePowerState)
        {
            continue;
        }
        if (rule.transitional)
        {
            return rule.hostState;
        }
        if (auto fromStatus = hostStateFromDeviceStatus(deviceStatus))
        {
            return fromStatus;
        }
        return rule.hostState;
    }
    return hostStateFromDeviceStatus(deviceStatus);
}

inline constexpr bool isUnknownHostState(std::string_view hostState)
{
    return hostState == HOST_STATE_UNKNOWN;
}

/**
 * @brief CurrentHostState to seed at startup from HOST_STATE|switch-host.
 *
 * bmcctld writes the row only after it has loaded the platform, so the
 * bridge may start before it exists. A missing row, or one whose fields map
 * to nothing, yields HOST_STATE_UNKNOWN rather than a guess: reporting a
 * powered-off Switch-Host as On would let the Rack Manager act on it.
 *
 * @param rowPresent       HOST_STATE|switch-host exists in STATE_DB
 * @param devicePowerState device_power_state field (ignored when absent)
 * @param deviceStatus     device_status field (ignored when absent)
 */
inline constexpr std::string_view initialHostState(bool rowPresent,
                                                   std::string_view devicePowerState,
                                                   std::string_view deviceStatus)
{
    if (!rowPresent)
    {
        return HOST_STATE_UNKNOWN;
    }
    if (auto mapped = mapHostState(devicePowerState, deviceStatus))
    {
        return *mapped;
    }
    return HOST_STATE_UNKNOWN;
}

/**
 * @brief Map a D-Bus Host or Chassis transition to a RACK_MANAGER_COMMAND.
 * @return Command string, or nullopt if the transition is not supported.
 */
inline constexpr std::optional<std::string_view>
    transitionToCommand(std::string_view transition)
{
    for (const auto& rule : TRANSITION_RULES)
    {
        if (rule.transition == transition)
        {
            return rule.command;
        }
    }
    return std::nullopt;
}

/// @return D-Bus transition bmcweb issues for a Redfish ResetType, if supported.
inline constexpr std::optional<std::string_view>
    resetTypeToTransition(std::string_view resetType)
{
    for (const auto& rule : TRANSITION_RULES)
    {
        if (!rule.resetType.empty() && rule.resetType == resetType)
        {
            return rule.transition;
        }
    }
    return std::nullopt;
}

inline constexpr bool isHostTransition(std::string_view transition)
{
    return transition.starts_with("xyz.openbmc_project.State.Host.Transition.") &&
           transitionToCommand(transition).has_value();
}

inline constexpr bool isChassisTransition(std::string_view transition)
{
    return transition.starts_with("xyz.openbmc_project.State.Chassis.Transition.") &&
           transitionToCommand(transition).has_value();
}

/// Derive xyz.openbmc_project.State.Chassis.PowerState from a HostState.
inline constexpr std::string_view hostStateToChassisPowerState(std::string_view hostState)
{
    if (hostState == HOST_STATE_UNKNOWN)
    {
        return CHASSIS_POWER_STATE_UNKNOWN;
    }
    if (hostState == HOST_STATE_RUNNING)
    {
        return CHASSIS_POWER_STATE_ON;
    }
    if (hostState == HOST_STATE_TRANSITIONING_TO_RUNNING)
    {
        return CHASSIS_POWER_STATE_TRANSITIONING_TO_ON;
    }
    if (hostState == HOST_STATE_TRANSITIONING_TO_OFF)
    {
        return CHASSIS_POWER_STATE_TRANSITIONING_TO_OFF;
    }
    return CHASSIS_POWER_STATE_OFF;
}

/// @return true for DONE / FAILED (command slot can be released).
inline constexpr bool isTerminalCommandStatus(std::string_view status)
{
    return status == CMD_STATUS_DONE || status == CMD_STATUS_FAILED;
}

} // namespace sonic::dbus_bridge::host_state
