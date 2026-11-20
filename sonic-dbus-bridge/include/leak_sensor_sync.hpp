///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2026 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// Author: Chinmoy Dey <chinmoy@nexthop.ai>
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

#pragma once

#include "types.hpp"

#include <optional>
#include <string>

namespace sonic::dbus_bridge::leak_sync
{

/**
 * @brief What to do with a LIQUID_COOLING_INFO|<name> snapshot
 */
enum class LeakSensorAction
{
    Create, ///< No D-Bus object yet: create, register and report it
    Update, ///< Object exists and the tracked state changed
    None    ///< Object exists and nothing tracked changed
};

/**
 * @brief State tracked per sensor for change detection
 *
 * The derived DetectorState plus sensor hardware health, so a change in
 * either drives a D-Bus PropertiesChanged signal.
 */
inline std::string trackedLeakState(const LeakSensorInfo& sensor)
{
    return sensor.detectorState() + (sensor.functional() ? "" : "|Fault");
}

/**
 * @brief Decide how to apply a leak sensor snapshot
 *
 * thermalctld can publish a sensor after the bridge's startup read, so a
 * sensor without a D-Bus object is created rather than dropped.
 *
 * @param objectExists A D-Bus object exists for the sensor
 * @param cachedState  Last tracked state, if any
 * @param newState     trackedLeakState() of the snapshot
 */
inline LeakSensorAction
    decideLeakSensorAction(bool objectExists,
                           const std::optional<std::string>& cachedState,
                           const std::string& newState)
{
    if (!objectExists)
    {
        return LeakSensorAction::Create;
    }
    if (!cachedState || *cachedState != newState)
    {
        return LeakSensorAction::Update;
    }
    return LeakSensorAction::None;
}

/**
 * @brief Should a newly created detector announce its first state?
 *
 * A new object appears with InterfacesAdded, which bmcweb's leak monitor
 * does not watch. A detector first seen in any state other than OK is
 * reported as a DetectorState change so subscribers learn about it.
 */
inline bool announceInitialLeakState(const LeakSensorInfo& sensor)
{
    return sensor.detectorState() != "OK";
}

} // namespace sonic::dbus_bridge::leak_sync
