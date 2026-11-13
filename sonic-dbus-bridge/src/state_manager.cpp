///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2024 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// Author: Chinmoy Dey <chinmoy@nexthop.ai>
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

#include "state_manager.hpp"
#include "host_state_mapping.hpp"
#include "logger.hpp"
#include "redis_state_publisher.hpp"
#include <boost/asio/post.hpp>
#include <sdbusplus/exception.hpp>
#include <cstring>

namespace sonic::dbus_bridge
{

namespace
{

// Thrown from the RequestedHostTransition / RequestedPowerTransition setters
// when a Switch-Host command cannot be accepted right now. sdbusplus turns it
// into the xyz.openbmc_project.Common.Error.Unavailable D-Bus error, which
// bmcweb reports as 409 PropertyValueExternalConflict. A plain std::exception
// would surface as org.freedesktop.DBus.Error.InvalidArgs and a 500.
struct UnavailableError final : public sdbusplus::exception::generated_exception
{
    const char* name() const noexcept override
    {
        return "xyz.openbmc_project.Common.Error.Unavailable";
    }

    const char* description() const noexcept override
    {
        return "The service is temporarily unavailable.";
    }

    const char* what() const noexcept override
    {
        return "xyz.openbmc_project.Common.Error.Unavailable: "
               "The service is temporarily unavailable.";
    }
};

// D-Bus interface names
constexpr const char* IFACE_STATE_HOST = "xyz.openbmc_project.State.Host";
constexpr const char* IFACE_STATE_CHASSIS = "xyz.openbmc_project.State.Chassis";

// D-Bus object paths
constexpr const char* OBJ_PATH_HOST = "/xyz/openbmc_project/state/host0";
constexpr const char* OBJ_PATH_CHASSIS = "/xyz/openbmc_project/state/chassis0";

// Async execution delay (milliseconds)
constexpr int EXEC_DELAY_MS = 100;

// Upper bound for bmcctld to move a command to DONE / FAILED. Covers
// power_on_delay (300s default) plus oper-status verification. After this
// the action slot is released so later requests are not blocked forever.
constexpr std::chrono::seconds COMMAND_TIMEOUT{600};

} // namespace

StateManager::StateManager(sdbusplus::asio::object_server& server,
                           boost::asio::io_context& io,
                           std::shared_ptr<RedisAdapter> redisAdapter)
    : server_(server), io_(io), redisAdapter_(std::move(redisAdapter)),
      currentHostState_(host_state::HOST_STATE_UNKNOWN),
      currentChassisPowerState_(host_state::CHASSIS_POWER_STATE_UNKNOWN),
      redisPublisher_(std::make_unique<RedisStatePublisher>()),
      actionTimer_(std::make_unique<boost::asio::steady_timer>(io)),
      commandTimer_(std::make_unique<boost::asio::steady_timer>(io))
{
    // Connect to Redis STATE_DB
    LOG_INFO("StateManager: Connecting to Redis STATE_DB...");
    if (!redisPublisher_->connect())
    {
        LOG_ERROR("StateManager: Failed to connect to Redis STATE_DB");
    }
    else
    {
        LOG_INFO("StateManager: Connected to Redis STATE_DB successfully");
    }
}

void StateManager::initializeFromDb()
{
    std::optional<HostStateInfo> info;
    if (redisAdapter_)
    {
        info = redisAdapter_->getHostState();
    }

    // Never guess: until bmcctld has published a recognisable row the host
    // state is reported as unknown (PowerState omitted on the Redfish side).
    // The first HOST_STATE|switch-host notification replaces it and emits
    // the matching host power event.
    std::string_view seeded = host_state::initialHostState(
        info.has_value(), info ? info->devicePowerState : std::string(),
        info ? info->deviceStatus : std::string());

    currentHostState_ = std::string(seeded);
    currentChassisPowerState_ =
        std::string(host_state::hostStateToChassisPowerState(seeded));

    if (!info)
    {
        LOG_WARNING("StateManager: %s not present in STATE_DB, reporting "
                    "the Switch-Host state as unknown until bmcctld publishes it",
                    host_state::KEY_HOST_STATE.data());
        return;
    }

    if (host_state::isUnknownHostState(seeded))
    {
        LOG_WARNING("StateManager: unrecognised %s (device_power_state=%s, "
                    "device_status=%s), reporting the Switch-Host state as "
                    "unknown until bmcctld publishes a recognised value",
                    host_state::KEY_HOST_STATE.data(),
                    info->devicePowerState.c_str(), info->deviceStatus.c_str());
        return;
    }

    LOG_NOTICE("Switch-Host initial state: device_power_state=%s "
               "device_status=%s -> %s",
               info->devicePowerState.c_str(), info->deviceStatus.c_str(),
               currentHostState_.c_str());
}

bool StateManager::createStateObjects()
{
    LOG_INFO( "Creating state objects...");

    initializeFromDb();

    try
    {
        // xyz.openbmc_project.State.Host on host0
        hostStateIface_ = server_.add_interface(OBJ_PATH_HOST, IFACE_STATE_HOST);

        hostStateIface_->register_property_rw<std::string>(
            "RequestedHostTransition",
            sdbusplus::vtable::property_::emits_change,
            [this](const std::string& newValue, const auto&) {
                LOG_NOTICE("Switch-Host power request: RequestedHostTransition=%s "
                           "(current host state: %s)",
                           newValue.c_str(), currentHostState_.c_str());
                queueTransition(newValue);
                lastRequestedHostTransition_ = newValue;
                return 1;
            },
            [this](const auto&) {
                return lastRequestedHostTransition_;
            });

        hostStateIface_->register_property_r<std::string>(
            "CurrentHostState",
            sdbusplus::vtable::property_::emits_change,
            [this](const auto&) {
                return currentHostState_;
            });

        // Advertised to bmcweb for ResetType@Redfish.AllowableValues
        std::vector<std::string> allowedHostTransitions = {
            std::string(host_state::HOST_TRANSITION_ON),
            std::string(host_state::HOST_TRANSITION_OFF),
        };
        hostStateIface_->register_property_r<std::vector<std::string>>(
            "AllowedHostTransitions",
            sdbusplus::vtable::property_::const_,
            [allowedHostTransitions](const auto&) {
                return allowedHostTransitions;
            });

        hostStateIface_->initialize();

        // xyz.openbmc_project.State.Chassis on chassis0 (ForceOff path)
        chassisStateIface_ =
            server_.add_interface(OBJ_PATH_CHASSIS, IFACE_STATE_CHASSIS);

        chassisStateIface_->register_property_rw<std::string>(
            "RequestedPowerTransition",
            sdbusplus::vtable::property_::emits_change,
            [this](const std::string& newValue, const auto&) {
                LOG_NOTICE("Switch-Host power request: RequestedPowerTransition=%s "
                           "(current host state: %s)",
                           newValue.c_str(), currentHostState_.c_str());
                queueTransition(newValue);
                lastRequestedChassisTransition_ = newValue;
                return 1;
            },
            [this](const auto&) {
                return lastRequestedChassisTransition_;
            });

        chassisStateIface_->register_property_r<std::string>(
            "CurrentPowerState",
            sdbusplus::vtable::property_::emits_change,
            [this](const auto&) {
                return currentChassisPowerState_;
            });

        chassisStateIface_->initialize();

        LOG_INFO( "Created state object at %s", OBJ_PATH_HOST);
        LOG_INFO( "Created state object at %s", OBJ_PATH_CHASSIS);
        LOG_INFO( "Initial state: %s", currentHostState_.c_str());
        return true;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR( "Failed to create state objects: %s", e.what());
        return false;
    }
}

void StateManager::queueTransition(const std::string& transition)
{
    if (!host_state::transitionToCommand(transition))
    {
        LOG_ERROR( "Invalid transition value: %s", transition.c_str());
        throw std::invalid_argument("Invalid transition value");
    }

    if (actionQueue_.size() >= MAX_QUEUE_SIZE)
    {
        // Client is sending faster than bmcctld completes commands. Not a
        // bridge fault, so warn rather than error.
        LOG_WARNING("Action queue full (size: %zu), rejecting request %s",
                    actionQueue_.size(), transition.c_str());
        throw UnavailableError();
    }

    ActionRequest request;
    request.transition = transition;
    request.timestamp = std::chrono::steady_clock::now();
    actionQueue_.push(request);

    LOG_INFO( "Action queued (queue size: %zu)", actionQueue_.size());

    processNextAction();
}

void StateManager::processNextAction()
{
    if (actionInProgress_)
    {
        LOG_DEBUG( "Action already in progress (pending %s), waiting...",
                   pendingCommandId_.c_str());
        return;
    }

    if (actionQueue_.empty())
    {
        return;
    }

    actionInProgress_ = true;

    ActionRequest action = actionQueue_.front();
    actionQueue_.pop();

    LOG_INFO( "Processing action: %s (remaining in queue: %zu)",
           action.transition.c_str(), actionQueue_.size());

    // Schedule async execution using timer (non-blocking)
    actionTimer_->expires_after(std::chrono::milliseconds(EXEC_DELAY_MS));
    actionTimer_->async_wait([this, transition = action.transition](
                                 const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted)
        {
            LOG_WARNING( "Action timer cancelled");
            actionInProgress_ = false;
            return;
        }

        if (ec)
        {
            LOG_ERROR( "Action timer error: %s", ec.message().c_str());
            actionInProgress_ = false;
            processNextAction();
            return;
        }

        // The slot stays busy until bmcctld reports DONE / FAILED (or the
        // command times out). See handleCommandStatus().
        if (!executeHostTransition(transition))
        {
            actionInProgress_ = false;
            processNextAction();
        }
    });
}

bool StateManager::executeHostTransition(const std::string& transition)
{
    LOG_INFO("=== Executing Host Transition ===");
    LOG_INFO("Transition: %s", transition.c_str());

    if (!redisPublisher_ || !redisPublisher_->isConnected())
    {
        LOG_ERROR("Redis publisher not connected, cannot publish transition %s",
                  transition.c_str());
        return false;
    }

    auto command = host_state::transitionToCommand(transition);
    if (!command)
    {
        LOG_ERROR("Failed to map transition %s to command", transition.c_str());
        return false;
    }

    std::string commandStr(*command);
    LOG_INFO("Publishing command '%s' to RACK_MANAGER_COMMAND...", commandStr.c_str());
    std::string commandId = redisPublisher_->publishHostRequest(commandStr);

    if (commandId.empty())
    {
        LOG_ERROR("Failed to publish RACK_MANAGER_COMMAND (%s) to Redis",
                  commandStr.c_str());
        return false;
    }

    LOG_NOTICE("Rack manager command %s|%s published: command=%s transition=%s "
               "(current host state: %s)",
               host_state::TABLE_RACK_MANAGER_COMMAND.data(), commandId.c_str(),
               commandStr.c_str(), transition.c_str(), currentHostState_.c_str());

    pendingCommandId_ = commandId;
    pendingCommand_ = commandStr;

    commandTimer_->expires_after(COMMAND_TIMEOUT);
    commandTimer_->async_wait(
        [this, commandId](const boost::system::error_code& ec) {
            if (ec == boost::asio::error::operation_aborted)
            {
                return;
            }
            onCommandTimeout(commandId);
        });

    return true;
}

void StateManager::onHostStateChanged(const HostStateInfo& info)
{
    boost::asio::post(io_, [this, info]() { applyHostState(info); });
}

void StateManager::onCommandStatusChanged(const RackManagerCommandInfo& info)
{
    boost::asio::post(io_, [this, info]() { handleCommandStatus(info); });
}

void StateManager::applyHostState(const HostStateInfo& info)
{
    auto mapped = host_state::mapHostState(info.devicePowerState,
                                           info.deviceStatus);
    if (!mapped)
    {
        LOG_WARNING("Unrecognised %s update (device_power_state=%s, "
                    "device_status=%s), keeping %s",
                    host_state::KEY_HOST_STATE.data(),
                    info.devicePowerState.c_str(), info.deviceStatus.c_str(),
                    currentHostState_.c_str());
        return;
    }

    LOG_INFO("Switch-Host state update: device_power_state=%s device_status=%s "
             "last_change=%s -> %s",
             info.devicePowerState.c_str(), info.deviceStatus.c_str(),
             info.lastChangeTimestamp.c_str(), mapped->data());

    updateHostState(std::string(*mapped));
}

void StateManager::handleCommandStatus(const RackManagerCommandInfo& info)
{
    if (info.commandId != pendingCommandId_)
    {
        LOG_DEBUG("Ignoring %s|%s update (status=%s), not the pending command",
                  host_state::TABLE_RACK_MANAGER_COMMAND.data(),
                  info.commandId.c_str(), info.status.c_str());
        return;
    }

    LOG_INFO("Rack manager command %s status: %s (command=%s result=%s)",
             info.commandId.c_str(), info.status.c_str(),
             info.command.c_str(), info.result.c_str());

    if (!host_state::isTerminalCommandStatus(info.status))
    {
        return;
    }

    if (info.status == host_state::CMD_STATUS_DONE)
    {
        LOG_NOTICE("Rack manager command %s (%s) completed: %s",
                   info.commandId.c_str(), info.command.c_str(),
                   info.result.c_str());
    }
    else
    {
        LOG_ERROR("Rack manager command %s (%s) failed: %s",
                  info.commandId.c_str(), info.command.c_str(),
                  info.result.empty() ? "no result reported" : info.result.c_str());
    }

    completePendingCommand();
}

void StateManager::onCommandTimeout(const std::string& commandId)
{
    if (commandId != pendingCommandId_)
    {
        return;
    }

    LOG_ERROR("Rack manager command %s (%s) did not reach DONE/FAILED within "
              "%llds, releasing action slot",
              commandId.c_str(), pendingCommand_.c_str(),
              static_cast<long long>(COMMAND_TIMEOUT.count()));

    completePendingCommand();
}

void StateManager::completePendingCommand()
{
    commandTimer_->cancel();
    pendingCommandId_.clear();
    pendingCommand_.clear();
    actionInProgress_ = false;
    processNextAction();
}

void StateManager::updateHostState(const std::string& newState)
{
    if (currentHostState_ == newState)
    {
        LOG_DEBUG("Host state unchanged (%s)", newState.c_str());
        return;
    }

    LOG_NOTICE("Switch-Host state change: %s -> %s",
               currentHostState_.c_str(), newState.c_str());

    currentHostState_ = newState;

    if (hostStateIface_)
    {
        hostStateIface_->signal_property("CurrentHostState");
    }

    std::string chassisPowerState(
        host_state::hostStateToChassisPowerState(newState));
    if (chassisPowerState != currentChassisPowerState_)
    {
        currentChassisPowerState_ = chassisPowerState;
        if (chassisStateIface_)
        {
            chassisStateIface_->signal_property("CurrentPowerState");
        }
    }
}

} // namespace sonic::dbus_bridge

