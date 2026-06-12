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
#include <regex>
#include <string>

namespace redfish
{

/**
 * @brief Handle POST .../Actions/SONiC.SubmitAlert
 *
 * Accepts the rack manager alert JSON and forwards it as-is to
 * sonic-dbus-bridge via D-Bus.  The bridge uses a declarative
 * field-mapping table to persist the data in Redis STATE_DB.
 *
 * The alert payload is wrapped under a top-level envelope key matching the
 * fixed "redfish.*" pattern (e.g. "redfish_alert_data", "redfishXYZ"); more
 * than one such envelope may be present and each is processed independently.
 *
 * Request body example (flat form):
 *   {
 *     "redfish_alert_data": {
 *       "LiquidPressureDeviation": {
 *         "LiquidPressure": 68,
 *         "Severity": "Major",
 *         "RscmPosition": 1
 *       },
 *       "LeakDetected": {
 *         "Severity": "Critical",
 *         "RscmPosition": 1
 *       },
 *       ...
 *     }
 *   }
 *
 * Wrapped form (ShutdownAlert) is also accepted; see
 * oem-extension/README.md for the full alert vocabulary.
 */
inline void handleSonicSubmitAlert(
    App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& managerId)
{
    // Common pre-flight: auth, manager ID, body size, JSON parse.
    auto reqJson =
        sonicOemValidateRequest(app, req, asyncResp, managerId, "SubmitAlert");
    if (!reqJson)
    {
        return;
    }

    // Require at least one top-level envelope key matching the fixed
    // (case-sensitive) "redfish.*" pattern (e.g. "redfish_alert_data",
    // "redfishXYZ").  The rack-manager firmware wraps every alert payload
    // (flat or ShutdownAlert form) under such an envelope; the bridge
    // recursively classifies the entries beneath each matching envelope.
    static const std::regex envelopePattern("redfish.*");
    bool hasEnvelope = false;
    if (reqJson->is_object())
    {
        for (const auto& item : reqJson->items())
        {
            if (std::regex_match(item.key(), envelopePattern))
            {
                hasEnvelope = true;
                break;
            }
        }
    }
    if (!hasEnvelope)
    {
        messages::propertyMissing(asyncResp->res, "redfish");
        return;
    }

    std::string jsonStr = reqJson->dump();

    BMCWEB_LOG_INFO("SONiC.SubmitAlert: forwarding {} bytes to D-Bus",
                    jsonStr.size());

    // Forward the raw JSON to sonic-dbus-bridge.
    // Transport errors (bridge down, name not owned, timeout) and
    // application errors (bridge returned false) are reported distinctly
    // so operators can tell "service unavailable" from "bad payload".
    crow::connections::systemBus->async_method_call(
        [asyncResp](const boost::system::error_code& ec, bool success) {
            if (ec)
            {
                BMCWEB_LOG_ERROR("SubmitAlert D-Bus transport error: {}",
                                 ec.message());
                messages::serviceTemporarilyUnavailable(asyncResp->res, "1");
                return;
            }
            if (!success)
            {
                BMCWEB_LOG_WARNING(
                    "SubmitAlert: bridge rejected payload");
                messages::operationFailed(asyncResp->res);
                return;
            }
            asyncResp->res.result(
                boost::beast::http::status::no_content);
        },
        sonic_oem::rackManagerBusName, sonic_oem::rackManagerObjectPath,
        sonic_oem::rackManagerInterface, "SubmitAlert", jsonStr);
}

inline void requestRoutesSonicSubmitAlert(App& app)
{
    BMCWEB_ROUTE(
        app,
        "/redfish/v1/Managers/<str>/Oem/SONiC/RackManager/Actions/SONiC.SubmitAlert")
        .privileges(redfish::privileges::postManager)
        .methods(boost::beast::http::verb::post)(
            std::bind_front(handleSonicSubmitAlert, std::ref(app)));
}

} // namespace redfish
