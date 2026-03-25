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
 * @brief Handle POST .../Actions/SONiC.SubmitTelemetry
 *
 * Rack manager sends periodic telemetry data to the switch BMC:
 *   - Inlet liquid temperature (Celsius)
 *   - Inlet liquid flow rate (liters per minute)
 *   - Inlet liquid pressure (kPa)
 *   - Leak detection status
 *
 * Request body:
 *   {
 *     "InletLiquidTemperatureCelsius": 28.5,
 *     "InletLiquidFlowRateLPM": 12.3,
 *     "InletLiquidPressureKPa": 150.0,
 *     "LeakDetected": false
 *   }
 */
inline void handleSonicSubmitTelemetry(
    const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& managerId)
{
    if (managerId != BMCWEB_REDFISH_MANAGER_URI_NAME)
    {
        messages::resourceNotFound(asyncResp->res, "Manager", managerId);
        return;
    }

    double inletLiquidTemperatureCelsius = 0.0;
    double inletLiquidFlowRateLPM = 0.0;
    double inletLiquidPressureKPa = 0.0;
    bool leakDetected = false;
    std::optional<std::string> timestamp;

    if (!json_util::readJsonAction(
            req, asyncResp->res, "InletLiquidTemperatureCelsius",
            inletLiquidTemperatureCelsius, "InletLiquidFlowRateLPM",
            inletLiquidFlowRateLPM, "InletLiquidPressureKPa",
            inletLiquidPressureKPa, "LeakDetected", leakDetected, "Timestamp",
            timestamp))
    {
        // readJsonAction already set the error response
        return;
    }

    BMCWEB_LOG_INFO(
        "SONiC.SubmitTelemetry: Temp={}C, Flow={}LPM, Pressure={}kPa, Leak={}",
        inletLiquidTemperatureCelsius, inletLiquidFlowRateLPM,
        inletLiquidPressureKPa, leakDetected);

    // TODO: Forward telemetry data to D-Bus service (e.g., sonic-dbus-bridge)
    // for sensor value updates and threshold monitoring.
    //
    // Example D-Bus calls:
    //   Set xyz.openbmc_project.Sensor.Value on inlet temperature sensor
    //   Set xyz.openbmc_project.Sensor.Value on flow rate sensor
    //   Set xyz.openbmc_project.Sensor.Value on pressure sensor
    //   Set leak detection state on leak detector object
    //
    // If leakDetected is true, the BMC can generate a Redfish event
    // to notify subscribed clients (e.g., the rack manager itself).

    messages::success(asyncResp->res);
}

inline void requestRoutesSonicSubmitTelemetry(RedfishService& service)
{
    REDFISH_SUB_ROUTE<
        "/redfish/v1/Managers/<str>/#/Oem/SONiC/RackManagerInterface/Actions/SONiC.SubmitTelemetry">(
        service, HttpVerb::Post)(handleSonicSubmitTelemetry);
}

} // namespace redfish
