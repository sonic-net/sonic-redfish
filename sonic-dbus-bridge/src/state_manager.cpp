///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2024 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

#include "state_manager.hpp"
#include "logger.hpp"
#include "redis_state_publisher.hpp"
#include <cstring>

namespace sonic::dbus_bridge
{

namespace
{

// D-Bus interface names
constexpr const char* IFACE_STATE_HOST = "xyz.openbmc_project.State.Host";

// D-Bus object paths
constexpr const char* OBJ_PATH_HOST = "/xyz/openbmc_project/state/host0";

// Host transition values
constexpr const char* HOST_TRANS_ON = "xyz.openbmc_project.State.Host.Transition.On";
constexpr const char* HOST_TRANS_OFF = "xyz.openbmc_project.State.Host.Transition.Off";
constexpr const char* HOST_TRANS_REBOOT = "xyz.openbmc_project.State.Host.Transition.Reboot";
constexpr const char* HOST_TRANS_FORCE_WARM_REBOOT = "xyz.openbmc_project.State.Host.Transition.ForceWarmReboot";
constexpr const char* HOST_TRANS_POWER_CYCLE = "xyz.openbmc_project.State.Host.Transition.PowerCycle";

// Host state values
constexpr const char* HOST_STATE_OFF = "xyz.openbmc_project.State.Host.HostState.Off";
constexpr const char* HOST_STATE_TRANSITIONING = "xyz.openbmc_project.State.Host.HostState.TransitioningToRunning";
constexpr const char* HOST_STATE_RUNNING = "xyz.openbmc_project.State.Host.HostState.Running";

// Async execution delay (milliseconds)
constexpr int EXEC_DELAY_MS = 100;

} // namespace

StateManager::StateManager(sdbusplus::asio::object_server& server,
                           boost::asio::io_context& io)
    : server_(server), io_(io),
      currentHostState_(HOST_STATE_RUNNING),
      redisPublisher_(std::make_unique<RedisStatePublisher>()),
      actionTimer_(std::make_unique<boost::asio::steady_timer>(io))
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

bool StateManager::createStateObjects()
{
    LOG_INFO( "Creating state objects...");

    try
    {
        // Create xyz.openbmc_project.State.Host interface
        hostStateIface_ = server_.add_interface(OBJ_PATH_HOST, IFACE_STATE_HOST);

        // Register RequestedHostTransition property (read-write)
        hostStateIface_->register_property_rw<std::string>(
            "RequestedHostTransition",
            sdbusplus::vtable::property_::emits_change,
            [this](const std::string& newValue, const auto&) {
                // Property setter callback
                LOG_INFO( "=== Property Change Detected ===");
                LOG_INFO( "RequestedHostTransition = %s", newValue.c_str());

                // Validate transition value
                if (!isValidTransition(newValue))
                {
                    LOG_ERROR( "Invalid transition value: %s", newValue.c_str());
                    throw std::invalid_argument("Invalid transition value");
                }

                // Check queue overflow
                if (actionQueue_.size() >= MAX_QUEUE_SIZE)
                {
                    LOG_ERROR( "Action queue full (size: %zu), rejecting request",
                           actionQueue_.size());
                    throw std::runtime_error("Action queue full");
                }

                // Store last requested transition
                lastRequestedTransition_ = newValue;

                // Queue action for async execution
                ActionRequest request;
                request.transition = newValue;
                request.timestamp = std::chrono::steady_clock::now();
                actionQueue_.push(request);

                LOG_INFO( "Action queued (queue size: %zu)", actionQueue_.size());

                // Trigger async processing
                processNextAction();

                return 1; // Success
            },
            [this](const auto&) {
                // Property getter callback
                return lastRequestedTransition_;
            });

        // Register CurrentHostState property (read-only)
        hostStateIface_->register_property_r<std::string>(
            "CurrentHostState",
            sdbusplus::vtable::property_::emits_change,
            [this](const auto&) {
                return currentHostState_;
            });

        // Initialize the interface
        hostStateIface_->initialize();

        LOG_INFO( "Created state object at %s", OBJ_PATH_HOST);
        LOG_INFO( "Initial state: %s", currentHostState_.c_str());
        return true;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR( "Failed to create state objects: %s", e.what());
        return false;
    }
}

void StateManager::processNextAction()
{
    // Check if action already in progress
    if (actionInProgress_)
    {
        LOG_DEBUG( "Action already in progress, waiting...");
        return;
    }

    // Check if queue is empty
    if (actionQueue_.empty())
    {
        return;
    }

    // Mark action as in progress
    actionInProgress_ = true;

    // Get next action from queue
    ActionRequest action = actionQueue_.front();
    actionQueue_.pop();

    LOG_INFO( "Processing action: %s (remaining in queue: %zu)",
           action.transition.c_str(), actionQueue_.size());

    // Update state to transitioning
    updateHostState(HOST_STATE_TRANSITIONING);

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
            updateHostState(HOST_STATE_RUNNING);
            processNextAction(); // Try next action
            return;
        }

        // Execute the transition
        executeHostTransition(transition);

        // Mark action as complete
        actionInProgress_ = false;

        // Process next action in queue
        processNextAction();
    });
}

void StateManager::executeHostTransition(const std::string& transition)
{
    LOG_INFO("=== Executing Host Transition ===");
    LOG_INFO("Transition: %s", transition.c_str());

    // Check if Redis publisher is connected
    if (!redisPublisher_ || !redisPublisher_->isConnected())
    {
        LOG_ERROR("Redis publisher not connected, cannot publish transition");
        updateHostState(HOST_STATE_RUNNING);
        return;
    }

    std::string command = transitionToScriptCommand(transition);
    if (command.empty())
    {
        LOG_ERROR("Failed to map transition to command");
        updateHostState(HOST_STATE_RUNNING);
        return;
    }

    // Publish to Redis STATE_DB
    LOG_INFO("Publishing command '%s' to RACK_MANAGER_COMMAND...", command.c_str());
    std::string commandId = redisPublisher_->publishHostRequest(command);

    if (commandId.empty())
    {
        LOG_ERROR("Failed to publish RACK_MANAGER_COMMAND to Redis");
        updateHostState(HOST_STATE_RUNNING);
        return;
    }

    LOG_INFO("RACK_MANAGER_COMMAND|%s published successfully", commandId.c_str());

    // Update state based on transition
    if (transition == HOST_TRANS_OFF)
    {
        updateHostState(HOST_STATE_OFF);
    }
    else
    {
        updateHostState(HOST_STATE_RUNNING);
    }
}


void StateManager::updateHostState(const std::string& newState)
{
    if (currentHostState_ == newState)
    {
        return; // No change
    }

    LOG_INFO( "=== State Change ===");
    LOG_INFO( "Old state: %s", currentHostState_.c_str());
    LOG_INFO( "New state: %s", newState.c_str());

    currentHostState_ = newState;

    // Emit PropertiesChanged signal
    if (hostStateIface_)
    {
        hostStateIface_->signal_property("CurrentHostState");
    }
}

std::string StateManager::transitionToScriptCommand(const std::string& transition)
{
    if (transition == HOST_TRANS_ON)
    {
        return "POWER_ON";
    }
    else if (transition == HOST_TRANS_OFF)
    {
        return "POWER_OFF";
    }
    else if (transition == HOST_TRANS_REBOOT ||
             transition == HOST_TRANS_POWER_CYCLE ||
             transition == HOST_TRANS_FORCE_WARM_REBOOT)
    {
        return "POWER_CYCLE";
    }
    else
    {
        LOG_ERROR( "Unknown transition: %s", transition.c_str());
        return "";
    }
}

bool StateManager::isValidTransition(const std::string& transition)
{
    return transition == HOST_TRANS_ON ||
           transition == HOST_TRANS_OFF ||
           transition == HOST_TRANS_REBOOT ||
           transition == HOST_TRANS_FORCE_WARM_REBOOT ||
           transition == HOST_TRANS_POWER_CYCLE;
}

} // namespace sonic::dbus_bridge

