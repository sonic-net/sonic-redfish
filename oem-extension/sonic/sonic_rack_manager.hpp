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
#include "error_messages.hpp"
#include "http_request.hpp"
#include "query.hpp"
#include "redfish.hpp"
#include "registries/privilege_registry.hpp"
#include "sub_request.hpp"

#include <boost/beast/http/verb.hpp>
#include <nlohmann/json.hpp>

#include <memory>
#include <string>

namespace redfish
{

/**
 * @brief Populate the SONiC RackManager OEM resource representation.
 *
 * Shared by both the OEM sub-route (which merges this fragment into the
 * Manager response) and the standalone GET route (which serves it at its
 * own URL). Callers are responsible for any route setup (auth / privilege)
 * that applies to their entry point.
 */
inline void populateSonicRackManager(
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

/**
 * @brief OEM sub-route handler: contributes the RackManager fragment under
 *        Oem/SONiC/RackManager when the parent Manager resource is fetched.
 *
 * Route setup/auth is already performed by the Manager GET handler before
 * the sub-route is dispatched, so this only populates the fragment.
 */
inline void handleGetSonicRackManager(
    const SubRequest& /*req*/,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& managerId)
{
    populateSonicRackManager(asyncResp, managerId);
}

/**
 * @brief Standalone GET handler for
 *        /redfish/v1/Managers/<str>/Oem/SONiC/RackManager
 *
 * Serves the RackManager OEM sub-resource as a directly addressable resource.
 */
inline void handleGetSonicRackManagerStandalone(
    App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& managerId)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }
    populateSonicRackManager(asyncResp, managerId);
}

inline void requestRoutesSonicRackManager(App& app, RedfishService& service)
{
    // OEM fragment: merged into the Manager response under Oem/SONiC.
    REDFISH_SUB_ROUTE<
        "/redfish/v1/Managers/<str>/#/Oem/SONiC/RackManager">(
        service, HttpVerb::Get)(handleGetSonicRackManager);

    // Standalone resource: directly reachable at its own URL.
    BMCWEB_ROUTE(
        app, "/redfish/v1/Managers/<str>/Oem/SONiC/RackManager")
        .privileges(redfish::privileges::getManager)
        .methods(boost::beast::http::verb::get)(
            std::bind_front(handleGetSonicRackManagerStandalone,
                            std::ref(app)));
}

} // namespace redfish
