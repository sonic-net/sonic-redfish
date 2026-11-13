///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2026 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// Author: Chinmoy Dey <chinmoy@nexthop.ai>
// License file: sonic-redfish/LICENSE
///////////////////////////////////////
//
// D-Bus signal watcher for Switch-Host power state changes.
// Converts CurrentHostState PropertiesChanged signals on
// xyz.openbmc_project.State.Host (host0, owned by sonic-dbus-bridge and
// mirrored from STATE_DB HOST_STATE|switch-host) into Redfish events using
// the ResourceEvent message registry (ResourcePoweredOn, ResourcePoweredOff,
// ResourcePoweringOn, ResourcePoweringOff).
//
// Events are delivered to subscribers via EventServiceManager::sendEvent()
// with OriginOfCondition set to the ComputerSystem resource.

#include "host_state_monitor.hpp"

#include "bmcweb_config.h"
#include "dbus_singleton.hpp"
#include "dbus_utility.hpp"
#include "event_service_manager.hpp"
#include "logging.hpp"

#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/message.hpp>

#include <algorithm>
#include <string>
#include <variant>

namespace redfish
{

// Map a fully-qualified HostState D-Bus enum value to a ResourceEvent
// registry MessageId and human-readable message.
// Returns false if the state does not warrant an event.
static bool mapHostStateToEvent(const std::string& stateEnum,
                                std::string& messageId, std::string& message,
                                const std::string& resourceName)
{
    static constexpr std::string_view prefix =
        "xyz.openbmc_project.State.Host.HostState.";
    if (!stateEnum.starts_with(prefix))
    {
        return false;
    }
    std::string_view state = std::string_view(stateEnum).substr(prefix.size());

    if (state == "Running")
    {
        messageId = "ResourceEvent.1.3.ResourcePoweredOn";
        message = "The resource `" + resourceName + "` has powered on.";
        return true;
    }
    if (state == "Off")
    {
        messageId = "ResourceEvent.1.3.ResourcePoweredOff";
        message = "The resource `" + resourceName + "` has powered off.";
        return true;
    }
    if (state == "TransitioningToRunning")
    {
        messageId = "ResourceEvent.1.3.ResourcePoweringOn";
        message = "The resource `" + resourceName + "` is powering on.";
        return true;
    }
    if (state == "TransitioningToOff")
    {
        messageId = "ResourceEvent.1.3.ResourcePoweringOff";
        message = "The resource `" + resourceName + "` is powering off.";
        return true;
    }
    // Quiesced, DiagnosticMode, Standby -- no power event
    return false;
}

static void onHostStatePropertiesChanged(sdbusplus::message_t& msg)
{
    BMCWEB_LOG_DEBUG("Handling State.Host PropertiesChanged signal");

    std::string interface;
    dbus::utility::DBusPropertiesMap props;
    std::vector<std::string> invalidProps;
    msg.read(interface, props, invalidProps);

    auto found = std::ranges::find_if(props, [](const auto& x) {
        return x.first == "CurrentHostState";
    });
    if (found == props.end())
    {
        return;
    }

    const std::string* newState = std::get_if<std::string>(&found->second);
    if (newState == nullptr)
    {
        BMCWEB_LOG_ERROR("CurrentHostState was not a string");
        return;
    }

    const std::string resourceName(BMCWEB_REDFISH_SYSTEM_URI_NAME);
    std::string messageId;
    std::string message;
    if (!mapHostStateToEvent(*newState, messageId, message, resourceName))
    {
        BMCWEB_LOG_DEBUG("Host state {} does not produce an event", *newState);
        return;
    }

    std::string originUri = "/redfish/v1/Systems/" + resourceName;

    nlohmann::json::object_t eventMessage;
    eventMessage["MessageId"] = messageId;
    eventMessage["MessageArgs"] = nlohmann::json::array_t{resourceName};
    eventMessage["Severity"] = "OK";
    eventMessage["MessageSeverity"] = "OK";
    eventMessage["Message"] = message;

    // Set OriginOfCondition as a proper Redfish reference object.
    // We pass empty origin to sendEvent() so it doesn't overwrite this
    // with a flat string.
    nlohmann::json::object_t originObj;
    originObj["@odata.id"] = originUri;
    eventMessage["OriginOfCondition"] = std::move(originObj);

    BMCWEB_LOG_INFO("Sending host power event: {} ({})", messageId, *newState);

    EventServiceManager::getInstance().sendEvent(
        std::move(eventMessage), std::string_view(), "ComputerSystem");
}

const std::string hostStateMatchStr =
    "type='signal',member='PropertiesChanged',"
    "interface='org.freedesktop.DBus.Properties',"
    "arg0='xyz.openbmc_project.State.Host',"
    "path='/xyz/openbmc_project/state/host0'";

DbusHostStateMonitor::DbusHostStateMonitor() :
    hostStateMonitor(*crow::connections::systemBus, hostStateMatchStr,
                     onHostStatePropertiesChanged)
{}

} // namespace redfish
