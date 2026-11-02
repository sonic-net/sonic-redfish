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
// D-Bus signal watcher for leak detector PropertiesChanged events.
// Converts state changes on xyz.openbmc_project.Inventory.Item.LeakDetector
// objects into Redfish events using the Environmental message registry
// (LeakDetectedCritical, LeakDetectedWarning, LeakDetectedNormal). A
// detector that becomes Unavailable (the sensor faulted or unreadable) is
// reported with ResourceEvent ResourceStatusChangedWarning, and
// ResourceStatusChangedOK when it is readable again.
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
#include <map>
#include <string>
#include <variant>

namespace redfish
{

// Last DetectorState seen per detector object path, so that leaving
// Unavailable can be told apart from an ordinary leak clearing. Starts empty
// after a bmcweb restart: the first change seen is then treated as an
// ordinary transition, and the Rack Manager is expected to re-read the
// LeakDetectors after any restart anyway.
static std::map<std::string, std::string>& lastDetectorStates()
{
    static std::map<std::string, std::string> states;
    return states;
}

static std::string leakDetectorOriginUri(const std::string& sensorName)
{
    // The chassis id must match the Chassis resource bmcweb serves for the
    // bridge's /xyz/openbmc_project/inventory/system/chassis object,
    // otherwise subscribers receive a URI this BMC does not serve.
    return "/redfish/v1/Chassis/chassis/ThermalSubsystem/LeakDetection/LeakDetectors/" +
           sensorName;
}

// ResourceEvent.1.3.0.ResourceStatusChanged{Warning,OK} for a detector
// entering or leaving Unavailable. Message text follows the registry entry.
static void sendDetectorStatusChanged(const std::string& sensorName,
                                      bool recovered)
{
    const std::string health = recovered ? "OK" : "Warning";
    nlohmann::json::object_t eventMessage;
    eventMessage["MessageId"] = recovered
                                    ? "ResourceEvent.1.3.0.ResourceStatusChangedOK"
                                    : "ResourceEvent.1.3.0.ResourceStatusChangedWarning";
    eventMessage["MessageArgs"] =
        nlohmann::json::array_t{sensorName, health};
    eventMessage["Severity"] = health;
    eventMessage["MessageSeverity"] = health;
    eventMessage["Message"] =
        recovered ? "The health of resource '" + sensorName +
                        "' has changed to OK."
                  : "The health of resource `" + sensorName +
                        "` has changed to Warning.";

    nlohmann::json::object_t originObj;
    originObj["@odata.id"] = leakDetectorOriginUri(sensorName);
    eventMessage["OriginOfCondition"] = std::move(originObj);

    BMCWEB_LOG_INFO("Sending leak detector status event: {} for sensor {}",
                    eventMessage["MessageId"].get<std::string>(), sensorName);

    EventServiceManager::getInstance().sendEvent(
        std::move(eventMessage), std::string_view(), "LeakDetector");
}

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

    const std::string state = getDbusEnumSuffix(*newState);
    std::string previous;
    auto& states = lastDetectorStates();
    if (auto it = states.find(objPath.str); it != states.end())
    {
        previous = it->second;
    }
    states[objPath.str] = state;

    if (state == "Unavailable")
    {
        sendDetectorStatusChanged(sensorName, /*recovered=*/false);
        return;
    }
    if (previous == "Unavailable")
    {
        // Readable again: report the health change, then the leak state
        // it came back with (LeakDetectedNormal when not leaking) so a
        // subscriber matching only on the leak messages learns it too.
        sendDetectorStatusChanged(sensorName, /*recovered=*/true);
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

    std::string originUri = leakDetectorOriginUri(sensorName);

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
