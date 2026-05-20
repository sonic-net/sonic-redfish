// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright SONiC Contributors
#pragma once

#include "sonic/sonic_rack_manager.hpp"
#include "sonic/sonic_submit_alert.hpp"
#include "sonic/sonic_submit_telemetry.hpp"

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
    requestRoutesSonicRackManager(service);
    requestRoutesSonicSubmitAlert(app);
    requestRoutesSonicSubmitTelemetry(app);
}

} // namespace redfish
