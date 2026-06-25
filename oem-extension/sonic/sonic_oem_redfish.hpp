///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2026 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

#pragma once

#include "sonic/sonic_rack_manager.hpp"
#include "sonic/sonic_submit_alert.hpp"
#include "sonic/sonic_submit_telemetry.hpp"

// Design decisions (why OEM actions, why JSON-blob over D-Bus, threading
// model) are documented in oem-extension/README.md § 6.

namespace redfish
{

/**
 * @brief Register all SONiC OEM Redfish routes.
 *
 * This is the single entry point called from redfish.cpp.
 * To add a new OEM API:
 *   1. Create a new header in sonic/
 *   2. Add #include and call its requestRoutes function here.
 *
 * @param app     Crow application for registering standalone action routes
 * @param service RedfishService for registering OEM sub-routes (fragments)
 */
inline void requestRoutesSonicOem(App& app, RedfishService& service)
{
    requestRoutesSonicRackManager(app, service);
    requestRoutesSonicSubmitAlert(app);
    requestRoutesSonicSubmitTelemetry(app);
}

} // namespace redfish
