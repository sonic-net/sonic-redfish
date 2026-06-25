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
#include "logging.hpp"
#include "query.hpp"
#include "registries/privilege_registry.hpp"
#include "sonic/sonic_oem_constants.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace redfish
{

/**
 * @brief Common pre-flight validation for SONiC OEM POST action handlers.
 *
 * Performs, in order:
 *   1. Redfish route setup (auth, privilege check).
 *   2. Manager ID check — 404 if not the local BMC.
 *   3. Body size guard — 413 PayloadTooLarge if body exceeds kMaxRequestBodyBytes.
 *   4. JSON parse — 400 MalformedJSON if the body is not valid JSON.
 *
 * @param app        Crow application (passed to setUpRedfishRoute).
 * @param req        Incoming HTTP request.
 * @param asyncResp  Async response object; error response is set on failure.
 * @param managerId  The <str> path parameter from the route.
 * @param actionName Short action name used in log messages (e.g. "SubmitAlert").
 *
 * @return The parsed JSON body on success, or std::nullopt if the response
 *         has already been set with an appropriate error.
 */
inline std::optional<nlohmann::json> sonicOemValidateRequest(
    App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& managerId,
    std::string_view actionName)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return std::nullopt;
    }

    if (managerId != BMCWEB_REDFISH_MANAGER_URI_NAME)
    {
        messages::resourceNotFound(asyncResp->res, "Manager", managerId);
        return std::nullopt;
    }

    if (req.body().size() > sonic_oem::kMaxRequestBodyBytes)
    {
        BMCWEB_LOG_WARNING(
            "SONiC.{}: rejected body of {} bytes (cap {})",
            actionName, req.body().size(), sonic_oem::kMaxRequestBodyBytes);
        messages::payloadTooLarge(asyncResp->res);
        return std::nullopt;
    }

    nlohmann::json reqJson = nlohmann::json::parse(req.body(), nullptr, false);
    if (reqJson.is_discarded())
    {
        messages::malformedJSON(asyncResp->res);
        return std::nullopt;
    }

    return reqJson;
}

} // namespace redfish
