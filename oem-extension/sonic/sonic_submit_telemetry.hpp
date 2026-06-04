///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2026 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

#pragma once

#include "dbus_singleton.hpp"
#include "sonic/sonic_oem_utils.hpp"

#include <boost/beast/http/status.hpp>

#include <memory>
#include <string>

namespace redfish
{

/**
 * @brief Handle POST .../Actions/SONiC.SubmitTelemetry
 *
 * Accepts the rack manager telemetry JSON and forwards it as-is to
 * sonic-dbus-bridge via D-Bus.  The bridge uses a declarative
 * field-mapping table to persist the data in Redis STATE_DB.
 *
 * Request body example:
 *   {
 *     "Alarms": {
 *       "EnergyValveActive": true,
 *       "InletTempDeviation": {
 *         "InletTemperature": 16.87,
 *         "Severity": "Normal"
 *       },
 *       ...
 *     }
 *   }
 */
inline void handleSonicSubmitTelemetry(
    App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& managerId)
{
    // Common pre-flight: auth, manager ID, body size, JSON parse.
    auto reqJson = sonicOemValidateRequest(app, req, asyncResp, managerId,
                                          "SubmitTelemetry");
    if (!reqJson)
    {
        return;
    }

    // Require the top-level key
    if (!reqJson->contains("Alarms"))
    {
        messages::propertyMissing(asyncResp->res, "Alarms");
        return;
    }

    std::string jsonStr = reqJson->dump();

    BMCWEB_LOG_INFO("SONiC.SubmitTelemetry: forwarding {} bytes to D-Bus",
                    jsonStr.size());

    // Forward the raw JSON to sonic-dbus-bridge.
    // Transport errors (bridge down, name not owned, timeout) and
    // application errors (bridge returned false) are reported distinctly
    // so operators can tell "service unavailable" from "bad payload".
    crow::connections::systemBus->async_method_call(
        [asyncResp](const boost::system::error_code& ec, bool success) {
            if (ec)
            {
                BMCWEB_LOG_ERROR("SubmitTelemetry D-Bus transport error: {}",
                                 ec.message());
                messages::serviceTemporarilyUnavailable(asyncResp->res, "1");
                return;
            }
            if (!success)
            {
                BMCWEB_LOG_WARNING(
                    "SubmitTelemetry: bridge rejected payload");
                messages::operationFailed(asyncResp->res);
                return;
            }
            asyncResp->res.result(
                boost::beast::http::status::no_content);
        },
        sonic_oem::rackManagerBusName, sonic_oem::rackManagerObjectPath,
        sonic_oem::rackManagerInterface, "SubmitTelemetry", jsonStr);
}

inline void requestRoutesSonicSubmitTelemetry(App& app)
{
    BMCWEB_ROUTE(
        app,
        "/redfish/v1/Managers/<str>/Oem/SONiC/RackManager/Actions/SONiC.SubmitTelemetry")
        .privileges(redfish::privileges::postManager)
        .methods(boost::beast::http::verb::post)(
            std::bind_front(handleSonicSubmitTelemetry, std::ref(app)));
}

} // namespace redfish
