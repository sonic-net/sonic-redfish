///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2026 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

#pragma once

#include <cstddef>

//
// Shared constants for SONiC OEM Redfish route handlers.
//
// Centralised so that the bus name, object path and interface are defined
// in exactly one place; the bridge side (sonic-dbus-bridge) advertises the
// same triple via its own header (rack_manager_receiver.hpp) and a
// compile-time check there guards against drift.
//

namespace redfish::sonic_oem
{

// D-Bus coordinates for the sonic-dbus-bridge rack manager receiver.
// The bridge claims `com.sonic.RackManager` as a dedicated well-known
// name; methods live under /com/sonic/RackManager on the interface of
// the same name.
inline constexpr const char* rackManagerBusName = "com.sonic.RackManager";
inline constexpr const char* rackManagerObjectPath = "/com/sonic/RackManager";
inline constexpr const char* rackManagerInterface = "com.sonic.RackManager";

// Hard upper bound on POST body size accepted by the SONiC OEM action
// routes. Rack-manager alert/telemetry payloads are small (a few KB);
// anything beyond this is rejected before being forwarded to D-Bus.
inline constexpr std::size_t kMaxRequestBodyBytes = 64 * 1024;  // 64 KiB

} // namespace redfish::sonic_oem
