///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2024 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// Author: Chinmoy Dey <chinmoy@nexthop.ai>
// License file: sonic-redfish/LICENSE
///////////////////////////////////////
//
// Redfish LeakDetection and LeakDetector endpoints:
//   GET /redfish/v1/Chassis/{id}/ThermalSubsystem/LeakDetection/
//
//   Canonical LeakDetectors URIs (LeakDetection v1.2.0 / Chassis v1.26.0+):
//   GET /redfish/v1/Chassis/{id}/LeakDetectors/
//   GET /redfish/v1/Chassis/{id}/LeakDetectors/{detectorId}/
//
//   Deprecated LeakDetectors URIs (still served for back-compat with
//   pre-2025.4 clients; DMTF marked these deprecated in LeakDetection
//   v1.2.0 in favor of the Chassis-level collection above):
//   GET .../ThermalSubsystem/LeakDetection/LeakDetectors/
//   GET .../ThermalSubsystem/LeakDetection/LeakDetectors/{detectorId}/
//
// Reads leak sensor data from D-Bus objects created by sonic-dbus-bridge
// at /xyz/openbmc_project/sensors/leak/<name>.

#pragma once

#include "app.hpp"
#include "async_resp.hpp"
#include "dbus_utility.hpp"
#include "error_messages.hpp"
#include "generated/enums/leak_detector.hpp"
#include "generated/enums/resource.hpp"
#include "http_request.hpp"
#include "logging.hpp"
#include "query.hpp"
#include "registries/privilege_registry.hpp"
#include "utils/chassis_utils.hpp"
#include "utils/dbus_utils.hpp"

#include <boost/beast/http/verb.hpp>
#include <boost/url/format.hpp>
#include <sdbusplus/message/native_types.hpp>

#include <array>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace redfish
{

constexpr std::string_view leakSensorBasePath =
    "/xyz/openbmc_project/sensors/leak";
constexpr std::array<std::string_view, 1> leakDetectorInterfaces = {
    "xyz.openbmc_project.Inventory.Item.LeakDetector"};
constexpr const char* leakDetectorService =
    "xyz.openbmc_project.Inventory.Manager";
constexpr const char* leakDetectorInterface =
    "xyz.openbmc_project.Inventory.Item.LeakDetector";
// Functional reflects the sensor hardware (thermalctld leak_sensor_status),
// not the leak state.
constexpr const char* leakDetectorOperationalStatusInterface =
    "xyz.openbmc_project.State.Decorator.OperationalStatus";

// Extract the trailing segment after the last '.' from a D-Bus enum string.
// e.g. "xyz.openbmc_project.Inventory.Item.LeakDetector.DetectorState.Critical"
//       -> "Critical"
inline std::string getDbusEnumSuffix(const std::string& dbusEnum)
{
    auto pos = dbusEnum.rfind('.');
    if (pos != std::string::npos && pos + 1 < dbusEnum.size())
    {
        return dbusEnum.substr(pos + 1);
    }
    return dbusEnum;
}

inline resource::Health detectorStateToHealth(const std::string& state)
{
    if (state == "Critical")
    {
        return resource::Health::Critical;
    }
    if (state == "Warning")
    {
        return resource::Health::Warning;
    }
    return resource::Health::OK;
}

// A detector the bridge cannot read (DetectorState Unavailable: thermalctld
// reports the sensor faulted or its leak state is N/A) or whose hardware is
// not functional must not look healthy to the Rack Manager: it is reported
// UnavailableOffline with at least Warning health. A leak that is still
// known keeps its own, higher, health.
inline bool leakDetectorUnavailable(const std::string& state, bool functional)
{
    return state == "Unavailable" || !functional;
}

inline void setLeakDetectorStatus(nlohmann::json& json, const std::string& state,
                                  bool functional)
{
    resource::Health health = detectorStateToHealth(state);
    if (leakDetectorUnavailable(state, functional))
    {
        json["Status"]["State"] = resource::State::UnavailableOffline;
        if (health == resource::Health::OK)
        {
            health = resource::Health::Warning;
        }
    }
    else
    {
        json["Status"]["State"] = resource::State::Enabled;
    }
    json["Status"]["Health"] = health;
}

// The LeakDetectors collection is served at two URI forms: the canonical
// Chassis-level location (LeakDetection v1.2.0 / Chassis v1.26.0+) and the
// deprecated nested location under ThermalSubsystem/LeakDetection, kept so
// existing (pre-2025.4) clients don't break. The form is bound at route
// registration and selects which URI tree the response links use; the same
// D-Bus data backs both forms.
enum class LeakDetectorsUriForm
{
    Canonical,
    Deprecated
};

inline std::string leakDetectorCollectionUri(LeakDetectorsUriForm form,
                                             const std::string& chassisId)
{
    if (form == LeakDetectorsUriForm::Canonical)
    {
        return std::format("/redfish/v1/Chassis/{}/LeakDetectors", chassisId);
    }
    return std::format(
        "/redfish/v1/Chassis/{}/ThermalSubsystem/LeakDetection/LeakDetectors",
        chassisId);
}

inline std::string leakDetectorMemberUri(LeakDetectorsUriForm form,
                                         const std::string& chassisId,
                                         const std::string& detectorId)
{
    return std::format("{}/{}", leakDetectorCollectionUri(form, chassisId),
                       detectorId);
}

// ---- GET /redfish/v1/Chassis/{id}/ThermalSubsystem/LeakDetection ----

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
        [asyncResp,
         chassisId](const std::optional<std::string>& validChassisPath) {
            if (!validChassisPath)
            {
                messages::resourceNotFound(asyncResp->res, "Chassis",
                                           chassisId);
                return;
            }

            asyncResp->res.jsonValue["@odata.type"] =
                "#LeakDetection.v1_1_0.LeakDetection";
            asyncResp->res.jsonValue["@odata.id"] = boost::urls::format(
                "/redfish/v1/Chassis/{}/ThermalSubsystem/LeakDetection",
                chassisId);
            asyncResp->res.jsonValue["Id"] = "LeakDetection";
            asyncResp->res.jsonValue["Name"] = "Leak Detection";
            asyncResp->res.jsonValue["LeakDetectors"]["@odata.id"] =
                boost::urls::format(
                    "/redfish/v1/Chassis/{}/ThermalSubsystem/LeakDetection/LeakDetectors",
                    chassisId);
            asyncResp->res.jsonValue["Status"]["State"] =
                resource::State::Enabled;
            asyncResp->res.jsonValue["Status"]["Health"] =
                resource::Health::OK;
        });
}

inline void requestRoutesLeakDetection(App& app)
{
    BMCWEB_ROUTE(
        app,
        "/redfish/v1/Chassis/<str>/ThermalSubsystem/LeakDetection/")
        .privileges(redfish::privileges::getLeakDetection)
        .methods(boost::beast::http::verb::get)(
            std::bind_front(handleLeakDetectionGet, std::ref(app)));
}

// ---- GET .../LeakDetection/LeakDetectors (collection) ----

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
        [asyncResp, chassisId,
         form](const std::optional<std::string>& validChassisPath) {
            if (!validChassisPath)
            {
                messages::resourceNotFound(asyncResp->res, "Chassis",
                                           chassisId);
                return;
            }

            asyncResp->res.jsonValue["@odata.type"] =
                "#LeakDetectorCollection.LeakDetectorCollection";
            asyncResp->res.jsonValue["@odata.id"] =
                leakDetectorCollectionUri(form, chassisId);
            asyncResp->res.jsonValue["Name"] = "Leak Detector Collection";

            dbus::utility::getSubTreePaths(
                std::string(leakSensorBasePath), 0, leakDetectorInterfaces,
                [asyncResp, chassisId, form](
                    const boost::system::error_code& ec,
                    const dbus::utility::MapperGetSubTreePathsResponse&
                        paths) {
                    if (ec)
                    {
                        if (ec.value() == boost::system::errc::io_error)
                        {
                            asyncResp->res.jsonValue["Members"] =
                                nlohmann::json::array();
                            asyncResp->res
                                .jsonValue["Members@odata.count"] = 0;
                            return;
                        }
                        BMCWEB_LOG_ERROR(
                            "D-Bus error getting leak detectors: {}",
                            ec.message());
                        messages::internalError(asyncResp->res);
                        return;
                    }

                    nlohmann::json& members =
                        asyncResp->res.jsonValue["Members"];
                    members = nlohmann::json::array();

                    for (const auto& path : paths)
                    {
                        sdbusplus::message::object_path objPath(path);
                        std::string detectorId = objPath.filename();
                        if (detectorId.empty())
                        {
                            continue;
                        }
                        nlohmann::json::object_t member;
                        member["@odata.id"] = leakDetectorMemberUri(
                            form, chassisId, detectorId);
                        members.emplace_back(std::move(member));
                    }
                    asyncResp->res.jsonValue["Members@odata.count"] =
                        members.size();
                });
        });
}

inline void requestRoutesLeakDetectorCollection(App& app)
{
    // Canonical URI (LeakDetection v1.2.0 / Chassis v1.26.0+)
    BMCWEB_ROUTE(app, "/redfish/v1/Chassis/<str>/LeakDetectors/")
        .privileges(redfish::privileges::getLeakDetectorCollection)
        .methods(boost::beast::http::verb::get)(
            std::bind_front(handleLeakDetectorCollectionGet, std::ref(app),
                            LeakDetectorsUriForm::Canonical));

    // Deprecated nested URI (kept for back-compat)
    BMCWEB_ROUTE(
        app,
        "/redfish/v1/Chassis/<str>/ThermalSubsystem/LeakDetection/LeakDetectors/")
        .privileges(redfish::privileges::getLeakDetectorCollection)
        .methods(boost::beast::http::verb::get)(
            std::bind_front(handleLeakDetectorCollectionGet, std::ref(app),
                            LeakDetectorsUriForm::Deprecated));
}

// ---- GET .../LeakDetectors/{detectorId} (individual) ----

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
        [asyncResp, chassisId, detectorId,
         form](const std::optional<std::string>& validChassisPath) {
            if (!validChassisPath)
            {
                messages::resourceNotFound(asyncResp->res, "Chassis",
                                           chassisId);
                return;
            }

            std::string dbusPath =
                std::string(leakSensorBasePath) + "/" + detectorId;

            dbus::utility::getAllProperties(
                leakDetectorService, dbusPath, leakDetectorInterface,
                [asyncResp, chassisId, detectorId, form, dbusPath](
                    const boost::system::error_code& ec,
                    const dbus::utility::DBusPropertiesMap& properties) {
                    if (ec)
                    {
                        messages::resourceNotFound(asyncResp->res,
                                                   "LeakDetector",
                                                   detectorId);
                        return;
                    }

                    asyncResp->res.jsonValue["@odata.type"] =
                        "#LeakDetector.v1_5_0.LeakDetector";
                    asyncResp->res.jsonValue["@odata.id"] =
                        leakDetectorMemberUri(form, chassisId, detectorId);
                    asyncResp->res.jsonValue["Id"] = detectorId;
                    asyncResp->res.jsonValue["Name"] =
                        "Leak Detector " + detectorId;

                    std::string state;
                    for (const auto& [key, val] : properties)
                    {
                        const std::string* strVal =
                            std::get_if<std::string>(&val);
                        if (strVal == nullptr)
                        {
                            continue;
                        }
                        if (key == "DetectorState")
                        {
                            state = getDbusEnumSuffix(*strVal);
                            asyncResp->res.jsonValue["DetectorState"] =
                                state;
                        }
                        else if (key == "Type")
                        {
                            asyncResp->res
                                .jsonValue["LeakDetectorType"] = *strVal;
                        }
                    }

                    // Status needs the sensor hardware health as well.
                    dbus::utility::getProperty<bool>(
                        leakDetectorService, dbusPath,
                        leakDetectorOperationalStatusInterface, "Functional",
                        [asyncResp, detectorId,
                         state](const boost::system::error_code& ec2,
                                bool functional) {
                            if (ec2)
                            {
                                // Interface not exported: nothing known
                                // against the hardware, so judge by the
                                // detector state alone.
                                BMCWEB_LOG_DEBUG(
                                    "No OperationalStatus for {}: {}",
                                    detectorId, ec2.message());
                                functional = true;
                            }
                            setLeakDetectorStatus(asyncResp->res.jsonValue,
                                                  state, functional);
                        });
                });
        });
}

inline void requestRoutesLeakDetector(App& app)
{
    // Canonical URI (LeakDetection v1.2.0 / Chassis v1.26.0+)
    BMCWEB_ROUTE(app, "/redfish/v1/Chassis/<str>/LeakDetectors/<str>/")
        .privileges(redfish::privileges::getLeakDetector)
        .methods(boost::beast::http::verb::get)(
            std::bind_front(handleLeakDetectorGet, std::ref(app),
                            LeakDetectorsUriForm::Canonical));

    // Deprecated nested URI (kept for back-compat)
    BMCWEB_ROUTE(
        app,
        "/redfish/v1/Chassis/<str>/ThermalSubsystem/LeakDetection/LeakDetectors/<str>/")
        .privileges(redfish::privileges::getLeakDetector)
        .methods(boost::beast::http::verb::get)(
            std::bind_front(handleLeakDetectorGet, std::ref(app),
                            LeakDetectorsUriForm::Deprecated));
}

} // namespace redfish
