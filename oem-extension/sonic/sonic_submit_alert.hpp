// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright SONiC Contributors
#pragma once

#include "app.hpp"
#include "async_resp.hpp"
#include "dbus_singleton.hpp"
#include "error_messages.hpp"
#include "http_request.hpp"
#include "logging.hpp"
#include "query.hpp"
#include "registries/privilege_registry.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>

namespace redfish
{

// D-Bus coordinates for the sonic-dbus-bridge rack manager receiver
constexpr const char* alertDbusService =
    "xyz.openbmc_project.Inventory.Manager";
constexpr const char* alertDbusObject =
    "/xyz/openbmc_project/sonic/rack_manager";
constexpr const char* alertDbusInterface = "com.sonic.RackManager";

/**
 * @brief Handle POST .../Actions/SONiC.SubmitAlert
 *
 * Accepts the rack manager alert JSON and forwards it as-is to
 * sonic-dbus-bridge via D-Bus.  The bridge uses a declarative
 * field-mapping table to persist the data in Redis STATE_DB.
 *
 * Request body example:
 *   {
 *     "redfish_alert_data": {
 *       "FlowRateDeviation": {
 *         "InletTemperature": 18,
 *         "FlowRate": 58,
 *         "Severity": "Minor",
 *         "RscmPosition": 1
 *       },
 *       ...
 *     }
 *   }
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

    // Validate that the body is valid JSON
    nlohmann::json reqJson =
        nlohmann::json::parse(req.body(), nullptr, false);
    if (reqJson.is_discarded())
    {
        messages::malformedJSON(asyncResp->res);
        return;
    }

    // Require the top-level key
    if (!reqJson.contains("redfish_alert_data"))
    {
        messages::propertyMissing(asyncResp->res, "redfish_alert_data");
        return;
    }

    std::string jsonStr = reqJson.dump();

    BMCWEB_LOG_INFO("SONiC.SubmitAlert: forwarding {} bytes to D-Bus",
                    jsonStr.size());

    // Forward the raw JSON to sonic-dbus-bridge
    crow::connections::systemBus->async_method_call(
        [asyncResp](const boost::system::error_code& ec, bool success) {
            if (ec)
            {
                BMCWEB_LOG_ERROR("SubmitAlert D-Bus error: {}", ec.message());
                messages::internalError(asyncResp->res);
                return;
            }
            if (!success)
            {
                BMCWEB_LOG_WARNING("SubmitAlert: bridge returned failure");
                messages::internalError(asyncResp->res);
                return;
            }
            messages::success(asyncResp->res);
        },
        alertDbusService, alertDbusObject, alertDbusInterface, "SubmitAlert",
        jsonStr);
}

inline void requestRoutesSonicSubmitAlert(App& app)
{
    BMCWEB_ROUTE(
        app,
        "/redfish/v1/Managers/<str>/Oem/SONiC/RackManagerInterface/Actions/SONiC.SubmitAlert/")
        .privileges(redfish::privileges::postManager)
        .methods(boost::beast::http::verb::post)(
            std::bind_front(handleSonicSubmitAlert, std::ref(app)));
}

} // namespace redfish
