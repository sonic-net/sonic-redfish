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
#include "leak_detection/leak_detector.hpp"
#include "leak_detection/leak_detector_model.hpp"
#include "utils/chassis_utils.hpp"

#include <boost/beast/http/field.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/url/format.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>

//
// DMTF-compliant LeakDetection / LeakDetectorCollection / LeakDetector
// route handlers registered at standard URIs.
//
// URI tree (chassis id is the wildcard segment):
//   GET /redfish/v1/Chassis/<id>/ThermalSubsystem/LeakDetection
//
//   Canonical LeakDetectors URIs (LeakDetection v1.2.0 / Chassis v1.26.0+):
//   GET /redfish/v1/Chassis/<id>/LeakDetectors
//   GET /redfish/v1/Chassis/<id>/LeakDetectors/<detectorId>
//
//   Deprecated LeakDetectors URIs (still served for back-compat with
//   pre-2025.4 clients; DMTF marked these deprecated in LeakDetection
//   v1.2.0 in favor of the Chassis-level collection above):
//   GET /redfish/v1/Chassis/<id>/ThermalSubsystem/LeakDetection/LeakDetectors
//   GET /redfish/v1/Chassis/<id>/ThermalSubsystem/LeakDetection/LeakDetectors/<detectorId>
//
// Schema versions emitted:
//   #LeakDetection.v1_1_0.LeakDetection
//   #LeakDetectorCollection.LeakDetectorCollection
//   #LeakDetector.v1_5_0.LeakDetector
//

namespace redfish
{

// ---------------------------------------------------------------------------
// LeakDetection resource ( /ThermalSubsystem/LeakDetection )
// ---------------------------------------------------------------------------

inline void doLeakDetectionGet(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& chassisId,
    const std::optional<std::string>& validChassisPath)
{
    if (!validChassisPath)
    {
        BMCWEB_LOG_WARNING("LeakDetection: unknown chassis '{}'", chassisId);
        messages::resourceNotFound(asyncResp->res, "Chassis", chassisId);
        return;
    }

    asyncResp->res.addHeader(
        boost::beast::http::field::link,
        "</redfish/v1/JsonSchemas/LeakDetection/LeakDetection.json>; rel=describedby");
    asyncResp->res.jsonValue["@odata.type"] =
        "#LeakDetection.v1_1_0.LeakDetection";
    asyncResp->res.jsonValue["@odata.id"] = boost::urls::format(
        "/redfish/v1/Chassis/{}/ThermalSubsystem/LeakDetection", chassisId);
    asyncResp->res.jsonValue["Id"] = "LeakDetection";
    asyncResp->res.jsonValue["Name"] = "Leak Detection";

    asyncResp->res.jsonValue["LeakDetectors"]["@odata.id"] =
        boost::urls::format(
            "/redfish/v1/Chassis/{}/ThermalSubsystem/LeakDetection/LeakDetectors",
            chassisId);

    asyncResp->res.jsonValue["Status"]["State"] = "Enabled";
    asyncResp->res.jsonValue["Status"]["Health"] =
        sonic_leak::aggregateDetectorHealth();
}

inline void handleLeakDetectionHead(
    App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& chassisId)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }
    auto respHandler = [asyncResp, chassisId](
                           const std::optional<std::string>& validChassisPath) {
        if (!validChassisPath)
        {
            messages::resourceNotFound(asyncResp->res, "Chassis", chassisId);
            return;
        }
        asyncResp->res.addHeader(
            boost::beast::http::field::link,
            "</redfish/v1/JsonSchemas/LeakDetection/LeakDetection.json>; rel=describedby");
    };
    redfish::chassis_utils::getValidChassisPath(asyncResp, chassisId,
                                                std::move(respHandler));
}

inline void handleLeakDetectionGet(
    App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& chassisId)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }
    redfish::chassis_utils::getValidChassisPath(
        asyncResp, chassisId,
        std::bind_front(doLeakDetectionGet, asyncResp, chassisId));
}

// ---------------------------------------------------------------------------
// Route registration
// ---------------------------------------------------------------------------

inline void requestRoutesLeakDetection(App& app)
{
    // LeakDetection resource
    BMCWEB_ROUTE(app,
                 "/redfish/v1/Chassis/<str>/ThermalSubsystem/LeakDetection/")
        .privileges(redfish::privileges::headLeakDetection)
        .methods(boost::beast::http::verb::head)(
            std::bind_front(handleLeakDetectionHead, std::ref(app)));

    BMCWEB_ROUTE(app,
                 "/redfish/v1/Chassis/<str>/ThermalSubsystem/LeakDetection/")
        .privileges(redfish::privileges::getLeakDetection)
        .methods(boost::beast::http::verb::get)(
            std::bind_front(handleLeakDetectionGet, std::ref(app)));

    // LeakDetectorCollection - canonical URI (LeakDetection v1.2.0+)
    BMCWEB_ROUTE(app, "/redfish/v1/Chassis/<str>/LeakDetectors/")
        .privileges(redfish::privileges::getLeakDetectorCollection)
        .methods(boost::beast::http::verb::get)(
            std::bind_front(handleLeakDetectorCollectionGet, std::ref(app),
                            LeakDetectorsUriForm::Canonical));

    // LeakDetector member - canonical URI
    BMCWEB_ROUTE(app, "/redfish/v1/Chassis/<str>/LeakDetectors/<str>/")
        .privileges(redfish::privileges::getLeakDetector)
        .methods(boost::beast::http::verb::get)(
            std::bind_front(handleLeakDetectorGet, std::ref(app),
                            LeakDetectorsUriForm::Canonical));

    // LeakDetectorCollection - deprecated nested URI (kept for back-compat)
    BMCWEB_ROUTE(
        app,
        "/redfish/v1/Chassis/<str>/ThermalSubsystem/LeakDetection/LeakDetectors/")
        .privileges(redfish::privileges::getLeakDetectorCollection)
        .methods(boost::beast::http::verb::get)(
            std::bind_front(handleLeakDetectorCollectionGet, std::ref(app),
                            LeakDetectorsUriForm::Deprecated));

    // LeakDetector member - deprecated nested URI (kept for back-compat)
    BMCWEB_ROUTE(
        app,
        "/redfish/v1/Chassis/<str>/ThermalSubsystem/LeakDetection/LeakDetectors/<str>/")
        .privileges(redfish::privileges::getLeakDetector)
        .methods(boost::beast::http::verb::get)(
            std::bind_front(handleLeakDetectorGet, std::ref(app),
                            LeakDetectorsUriForm::Deprecated));
}

} // namespace redfish
