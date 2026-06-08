///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2026 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

#pragma once

#include <array>
#include <cstddef>
#include <string_view>

//
// Static inventory model for the LeakDetection subsystem.
//
//

namespace redfish::sonic_leak
{

struct LeakDetectorEntry
{
    // URL segment and Redfish `Id`. Must be stable across reboots so
    // clients can hard-code references; treat as part of the public
    // contract.
    std::string_view id;

    // Human-readable `Name`.
    std::string_view name;

    // DMTF LeakDetector.LeakDetectorType enumeration:
    // either "Moisture" or "FloatSwitch".
    std::string_view leakDetectorType;

    // DMTF PhysicalContext enumeration (e.g. "Intake", "Exhaust",
    // "Chassis"). Identifies the region of the equipment the detector
    // watches.
    std::string_view physicalContext;

    // DMTF Resource.Health enumeration: "OK" | "Warning" | "Critical".
    // `DetectorState` is excerpt-typed to Health in v1_4_0 of the
    // schema, so the same value also drives `Status.Health`.
    std::string_view detectorState;
};

// Detector table. Two detectors are exposed: one watching the
// cold-aisle intake plenum, one watching the hot-aisle exhaust.
inline constexpr std::array<LeakDetectorEntry, 2> kLeakDetectors = {
    LeakDetectorEntry{
        "LeakDetector1", "Inlet Leak Detector",
        "Moisture", "Intake", "OK"},
    LeakDetectorEntry{
        "LeakDetector2", "Outlet Leak Detector",
        "Moisture", "Exhaust", "OK"},
};

// Returns a pointer to the entry matching `id`, or nullptr if no
// detector with that id exists.
inline const LeakDetectorEntry* findLeakDetector(std::string_view id)
{
    for (const auto& d : kLeakDetectors)
    {
        if (d.id == id)
        {
            return &d;
        }
    }
    return nullptr;
}

// Aggregate health of the detector set, surfaced on the LeakDetection
inline std::string_view aggregateDetectorHealth()
{
    std::string_view worst = "OK";
    for (const auto& d : kLeakDetectors)
    {
        if (d.detectorState == "Critical")
        {
            return "Critical";
        }
        if (d.detectorState == "Warning")
        {
            worst = "Warning";
        }
    }
    return worst;
}

} // namespace redfish::sonic_leak
