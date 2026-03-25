// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright SONiC Contributors
#pragma once

#include "app.hpp"
#include "async_resp.hpp"
#include "error_messages.hpp"
#include "http_request.hpp"
#include "logging.hpp"
#include "redfish.hpp"
#include "utils/json_utils.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <optional>
#include <string>

namespace redfish
{

/**
 * @brief Handle POST .../Actions/SONiC.SubmitAlert
 *
 * Rack manager posts a critical alert to the switch BMC.
 *
 * Request body:
 *   {
 *     "AlertType": "HighTemperature",
 *     "Severity": "Critical",
 *     "Message": "Inlet liquid temperature exceeded threshold"
 *   }
 */
inline void handleSonicSubmitAlert(
    const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& managerId)
{
    if (managerId != BMCWEB_REDFISH_MANAGER_URI_NAME)
    {
        messages::resourceNotFound(asyncResp->res, "Manager", managerId);
        return;
    }

    std::string alertType;
    std::string severity;
    std::string message;
    std::optional<std::string> originatorId;
    std::optional<std::string> timestamp;

    if (!json_util::readJsonAction(req, asyncResp->res, "AlertType", alertType,
                                   "Severity", severity, "Message", message,
                                   "OriginatorId", originatorId, "Timestamp",
                                   timestamp))
    {
        // readJsonAction already set the error response
        return;
    }

    // Validate Severity
    if (severity != "Critical" && severity != "Warning" && severity != "OK")
    {
        messages::propertyValueNotInList(asyncResp->res, severity, "Severity");
        return;
    }

    BMCWEB_LOG_INFO(
        "SONiC.SubmitAlert: AlertType={}, Severity={}, Message={}", alertType,
        severity, message);

    // TODO: Forward alert to D-Bus service (e.g., phosphor-logging or
    // sonic-dbus-bridge) for persistent storage and event propagation.
    //
    // Example D-Bus call:
    //   xyz.openbmc_project.Logging.Create(
    //       message, severity, additionalData)

    messages::success(asyncResp->res);
}

inline void requestRoutesSonicSubmitAlert(RedfishService& service)
{
    REDFISH_SUB_ROUTE<
        "/redfish/v1/Managers/<str>/#/Oem/SONiC/RackManagerInterface/Actions/SONiC.SubmitAlert">(
        service, HttpVerb::Post)(handleSonicSubmitAlert);
}

} // namespace redfish
