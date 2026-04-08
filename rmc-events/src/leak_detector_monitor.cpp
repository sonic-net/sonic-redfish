///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2024 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// License file: sonic-redfish/LICENSE
///////////////////////////////////////
//
// D-Bus signal watcher for leak detector PropertiesChanged events.
// Converts state changes on xyz.openbmc_project.Inventory.Item.LeakDetector
// objects into Redfish events using the Environmental message registry
// (LeakDetectedCritical, LeakDetectedWarning, LeakDetectedNormal).
//
// Events are delivered to subscribers via EventServiceManager::sendEvent().

#include "leak_detector_monitor.hpp"

#include "dbus_singleton.hpp"
#include "dbus_utility.hpp"
#include "event_service_manager.hpp"
#include "leak_detection.hpp"
#include "logging.hpp"

#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/message/native_types.hpp>

#include <algorithm>
#include <string>
#include <variant>

namespace redfish
{

// Map a fully-qualified DetectorState D-Bus enum value to an Environmental
// registry MessageId, severity, and human-readable message.
// Returns false if the state does not warrant an event (e.g. Unavailable).
static bool mapDetectorStateToEvent(const std::string& stateEnum,
                                    std::string& messageId,
                                    std::string& severity,
                                    std::string& message,
                                    const std::string& sensorName)
{
    std::string state = getDbusEnumSuffix(stateEnum);

    if (state == "Critical")
    {
        messageId = "Environmental.1.1.0.LeakDetectedCritical";
        severity = "Critical";
        message = "Leak detector '" + sensorName +
                  "' reports a critical level leak.";
        return true;
    }
    if (state == "Warning")
    {
        messageId = "Environmental.1.1.0.LeakDetectedWarning";
        severity = "Warning";
        message = "Leak detector '" + sensorName +
                  "' reports a warning level leak.";
        return true;
    }
    if (state == "OK")
    {
        messageId = "Environmental.1.1.0.LeakDetectedNormal";
        severity = "OK";
        message = "Leak detector '" + sensorName + "' has returned to normal.";
        return true;
    }
    // Unavailable, Absent, etc. -- no event
    return false;
}

static void onLeakDetectorPropertiesChanged(sdbusplus::message_t& msg)
{
    BMCWEB_LOG_DEBUG("Handling LeakDetector PropertiesChanged signal");

    sdbusplus::message::object_path objPath(msg.get_path());
    std::string interface;
    dbus::utility::DBusPropertiesMap props;
    std::vector<std::string> invalidProps;
    msg.read(interface, props, invalidProps);

    auto found = std::ranges::find_if(props, [](const auto& x) {
        return x.first == "DetectorState";
    });
    if (found == props.end())
    {
        return;
    }

    const std::string* newState =
        std::get_if<std::string>(&found->second);
    if (newState == nullptr)
    {
        BMCWEB_LOG_ERROR("DetectorState was not a string");
        return;
    }

    std::string sensorName = objPath.filename();
    if (sensorName.empty())
    {
        BMCWEB_LOG_ERROR("Could not extract sensor name from path");
        return;
    }

    std::string messageId;
    std::string severity;
    std::string message;
    if (!mapDetectorStateToEvent(*newState, messageId, severity, message,
                                 sensorName))
    {
        BMCWEB_LOG_DEBUG("State {} does not produce an event", *newState);
        return;
    }

    // Build origin URI -- use "BMC" as the chassis id for SONiC BMC
    std::string originUri =
        "/redfish/v1/Chassis/BMC/ThermalSubsystem/LeakDetection/LeakDetectors/" +
        sensorName;

    nlohmann::json::object_t eventMessage;
    eventMessage["MessageId"] = messageId;
    eventMessage["MessageArgs"] = nlohmann::json::array_t{sensorName};
    eventMessage["Severity"] = severity;
    eventMessage["Message"] = message;

    // Set OriginOfCondition as a proper Redfish reference object.
    // We pass empty origin to sendEvent() so it doesn't overwrite this
    // with a flat string.
    nlohmann::json::object_t originObj;
    originObj["@odata.id"] = originUri;
    eventMessage["OriginOfCondition"] = std::move(originObj);

    BMCWEB_LOG_INFO("Sending leak event: {} for sensor {}", messageId,
                    sensorName);

    EventServiceManager::getInstance().sendEvent(
        std::move(eventMessage), std::string_view(), "LeakDetector");
}

const std::string leakDetectorMatchStr =
    "type='signal',member='PropertiesChanged',"
    "interface='org.freedesktop.DBus.Properties',"
    "arg0='xyz.openbmc_project.Inventory.Item.LeakDetector',"
    "path_namespace='/xyz/openbmc_project/sensors/leak'";

DbusLeakDetectorMonitor::DbusLeakDetectorMonitor() :
    leakDetectorMonitor(*crow::connections::systemBus,
                         leakDetectorMatchStr,
                         onLeakDetectorPropertiesChanged)
{}

} // namespace redfish
