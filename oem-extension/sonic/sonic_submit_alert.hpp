///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2026 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

#pragma once

#include "app.hpp"
#include "async_resp.hpp"
#include "dbus_singleton.hpp"
#include "error_messages.hpp"
#include "http_request.hpp"
#include "logging.hpp"
#include "query.hpp"
#include "registries/privilege_registry.hpp"
#include "sonic/sonic_oem_constants.hpp"

#include <boost/beast/http/status.hpp>
#include <nlohmann/json.hpp>

#include <memory>
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
 * Request body example (flat form):
 *   {
 *     "Redfish": {
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
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }

    if (managerId != BMCWEB_REDFISH_MANAGER_URI_NAME)
    {
        messages::resourceNotFound(asyncResp->res, "Manager", managerId);
        return;
    }

    // Reject oversized payloads before parsing
    if (req.body().size() > sonic_oem::kMaxRequestBodyBytes)
    {
        BMCWEB_LOG_WARNING(
            "SONiC.SubmitAlert: rejected body of {} bytes (cap {})",
            req.body().size(), sonic_oem::kMaxRequestBodyBytes);
        asyncResp->res.result(
            boost::beast::http::status::payload_too_large);
        return;
    }

    // Validate that the body is valid JSON
    nlohmann::json reqJson =
        nlohmann::json::parse(req.body(), nullptr, false);
    if (reqJson.is_discarded())
    {
        messages::malformedJSON(asyncResp->res);
        return;
    }

    // Require the top-level key.  The rack-manager firmware wraps every
    // alert payload (flat or ShutdownAlert form) under "Redfish"; the
    // bridge's field_mapping paths assume that envelope.
    if (!reqJson.contains("Redfish"))
    {
        messages::propertyMissing(asyncResp->res, "Redfish");
        return;
    }

    std::string jsonStr = reqJson.dump();

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
