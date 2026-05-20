// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright SONiC Contributors
#pragma once

#include "async_resp.hpp"
#include "redfish.hpp"
#include "sub_request.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>

namespace redfish
{

/**
 * @brief Handle GET /redfish/v1/Managers/<str>/Oem/SONiC/RackManager
 *
 * Returns the SONiC RackManager OEM sub-resource, which describes
 * the available actions that a rack manager can invoke on this BMC.
 */
inline void handleGetSonicRackManager(
    const SubRequest& /*req*/,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& managerId)
{
    if (managerId != BMCWEB_REDFISH_MANAGER_URI_NAME)
    {
        messages::resourceNotFound(asyncResp->res, "Manager", managerId);
        return;
    }

    nlohmann::json& json = asyncResp->res.jsonValue;
    json["@odata.type"] = "#SonicManager.v1_0_0.RackManager";
    json["@odata.id"] =
        "/redfish/v1/Managers/" + std::string(BMCWEB_REDFISH_MANAGER_URI_NAME) +
        "/Oem/SONiC/RackManager";
    json["Id"] = "RackManager";
    json["Name"] = "SONiC Rack Manager Interface";
    json["Description"] =
        "OEM interface for rack manager to communicate with switch BMC";

    // Advertise available actions
    nlohmann::json& actions = json["Actions"];
    actions["#SONiC.SubmitAlert"]["target"] =
        "/redfish/v1/Managers/" + std::string(BMCWEB_REDFISH_MANAGER_URI_NAME) +
        "/Oem/SONiC/RackManager/Actions/SONiC.SubmitAlert";

    actions["#SONiC.SubmitTelemetry"]["target"] =
        "/redfish/v1/Managers/" + std::string(BMCWEB_REDFISH_MANAGER_URI_NAME) +
        "/Oem/SONiC/RackManager/Actions/SONiC.SubmitTelemetry";
}

inline void requestRoutesSonicRackManager(RedfishService& service)
{
    REDFISH_SUB_ROUTE<
        "/redfish/v1/Managers/<str>/#/Oem/SONiC/RackManager">(
        service, HttpVerb::Get)(handleGetSonicRackManager);
}

} // namespace redfish
