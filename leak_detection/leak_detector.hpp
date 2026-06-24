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
#include "leak_detection/leak_detector_model.hpp"
#include "utils/chassis_utils.hpp"

#include <boost/beast/http/field.hpp>
#include <boost/beast/http/verb.hpp>
#include <nlohmann/json.hpp>

#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

//
// Handlers for the LeakDetectorCollection and individual LeakDetector
// resources. Two URI forms are served:
//
//   Canonical  (LeakDetection v1.2.0 / Chassis v1.26.0+, DMTF 2025.4):
//     /redfish/v1/Chassis/<id>/LeakDetectors[/<detectorId>]
//
//   Deprecated (still valid; pre-LeakDetection v1.2.0):
//     /redfish/v1/Chassis/<id>/ThermalSubsystem/LeakDetection/LeakDetectors[/<detectorId>]
//

namespace redfish
{

// Which URI tree to use when building self/member links for a given
// request. Bound by route registration in leak_detection.hpp.
enum class LeakDetectorsUriForm
{
    Canonical,
    Deprecated,
};

inline std::string leakDetectorsCollectionUri(
    const std::string& chassisId, LeakDetectorsUriForm form)
{
    if (form == LeakDetectorsUriForm::Canonical)
    {
        return std::format("/redfish/v1/Chassis/{}/LeakDetectors", chassisId);
    }
    return std::format(
        "/redfish/v1/Chassis/{}/ThermalSubsystem/LeakDetection/LeakDetectors",
        chassisId);
}

inline std::string leakDetectorMemberUri(
    const std::string& chassisId, std::string_view detectorId,
    LeakDetectorsUriForm form)
{
    return std::format("{}/{}", leakDetectorsCollectionUri(chassisId, form),
                       detectorId);
}

// ---------------------------------------------------------------------------
// LeakDetectorCollection
// ---------------------------------------------------------------------------

inline void doLeakDetectorCollectionGet(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& chassisId, LeakDetectorsUriForm form,
    const std::optional<std::string>& validChassisPath)
{
    if (!validChassisPath)
    {
        BMCWEB_LOG_WARNING("LeakDetectors: unknown chassis '{}'", chassisId);
        messages::resourceNotFound(asyncResp->res, "Chassis", chassisId);
        return;
    }

    asyncResp->res.addHeader(
        boost::beast::http::field::link,
        "</redfish/v1/JsonSchemas/LeakDetectorCollection/LeakDetectorCollection.json>; rel=describedby");
    asyncResp->res.jsonValue["@odata.type"] =
        "#LeakDetectorCollection.LeakDetectorCollection";
    asyncResp->res.jsonValue["@odata.id"] =
        leakDetectorsCollectionUri(chassisId, form);
    asyncResp->res.jsonValue["Name"] = "Leak Detector Collection";

    nlohmann::json::array_t members;
    members.reserve(sonic_leak::kLeakDetectors.size());
    for (const auto& d : sonic_leak::kLeakDetectors)
    {
        nlohmann::json m;
        m["@odata.id"] = leakDetectorMemberUri(chassisId, d.id, form);
        members.emplace_back(std::move(m));
    }
    asyncResp->res.jsonValue["Members@odata.count"] = members.size();
    asyncResp->res.jsonValue["Members"] = std::move(members);
}

inline void handleLeakDetectorCollectionGet(
    App& app, LeakDetectorsUriForm form, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& chassisId)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }
    redfish::chassis_utils::getValidChassisPath(
        asyncResp, chassisId,
        std::bind_front(doLeakDetectorCollectionGet, asyncResp, chassisId,
                        form));
}

// ---------------------------------------------------------------------------
// LeakDetector member
// ---------------------------------------------------------------------------

inline void doLeakDetectorGet(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& chassisId, const std::string& detectorId,
    LeakDetectorsUriForm form,
    const std::optional<std::string>& validChassisPath)
{
    if (!validChassisPath)
    {
        BMCWEB_LOG_WARNING("LeakDetector: unknown chassis '{}'", chassisId);
        messages::resourceNotFound(asyncResp->res, "Chassis", chassisId);
        return;
    }

    const auto* entry = sonic_leak::findLeakDetector(detectorId);
    if (entry == nullptr)
    {
        messages::resourceNotFound(asyncResp->res, "LeakDetector", detectorId);
        return;
    }

    asyncResp->res.addHeader(
        boost::beast::http::field::link,
        "</redfish/v1/JsonSchemas/LeakDetector/LeakDetector.json>; rel=describedby");
    asyncResp->res.jsonValue["@odata.type"] =
        "#LeakDetector.v1_5_0.LeakDetector";
    asyncResp->res.jsonValue["@odata.id"] =
        leakDetectorMemberUri(chassisId, detectorId, form);
    asyncResp->res.jsonValue["Id"] = std::string(entry->id);
    asyncResp->res.jsonValue["Name"] = std::string(entry->name);
    asyncResp->res.jsonValue["LeakDetectorType"] =
        std::string(entry->leakDetectorType);
    asyncResp->res.jsonValue["PhysicalContext"] =
        std::string(entry->physicalContext);
    asyncResp->res.jsonValue["DetectorState"] =
        std::string(entry->detectorState);
    asyncResp->res.jsonValue["Status"]["State"] = "Enabled";
    asyncResp->res.jsonValue["Status"]["Health"] =
        std::string(entry->detectorState);
}

inline void handleLeakDetectorGet(
    App& app, LeakDetectorsUriForm form, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& chassisId, const std::string& detectorId)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }
    redfish::chassis_utils::getValidChassisPath(
        asyncResp, chassisId,
        std::bind_front(doLeakDetectorGet, asyncResp, chassisId, detectorId,
                        form));
}

} // namespace redfish
