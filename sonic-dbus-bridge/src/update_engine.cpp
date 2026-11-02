///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2024 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// Author: Chinmoy Dey <chinmoy@nexthop.ai>
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

#include "update_engine.hpp"
#include "host_state_mapping.hpp"
#include "inventory_model.hpp"
#include "leak_sensor_sync.hpp"
#include "logger.hpp"

#include <boost/asio/post.hpp>

namespace sonic::dbus_bridge
{

UpdateEngine::UpdateEngine(boost::asio::io_context& io,
                           std::shared_ptr<RedisAdapter> redisAdapter,
                           std::shared_ptr<DBusExporter> dbusExporter,
                           int pollIntervalSec)
    : io_(io), redisAdapter_(redisAdapter), dbusExporter_(dbusExporter),
      pollIntervalSec_(pollIntervalSec), timer_(io)
{
}

void UpdateEngine::start()
{
    if (running_)
    {
        return;
    }

    if (pollIntervalSec_ > 0)
    {
        LOG_INFO("Starting update engine (poll interval: %ds)", pollIntervalSec_);
        running_ = true;
        scheduleNextPoll();
    }
    else
    {
        LOG_INFO("Update engine started (event-driven mode, polling disabled)");
        running_ = true;
    }
}

void UpdateEngine::stop()
{
    if (!running_)
    {
        return;
    }

    LOG_INFO("Stopping update engine");
    running_ = false;
    timer_.cancel();
}

void UpdateEngine::onPollTimer(const boost::system::error_code& ec)
{
    if (ec == boost::asio::error::operation_aborted)
    {
        return; // Timer was cancelled
    }

    if (ec)
    {
        LOG_ERROR("Poll timer error: %s", ec.message().c_str());
        scheduleNextPoll();
        return;
    }

    doUpdate();
    scheduleNextPoll();
}

void UpdateEngine::doUpdate()
{
    try
    {
        // Read current data from Redis
        auto metadata = redisAdapter_->getDeviceMetadata();
        auto state = redisAdapter_->getChassisState();
        
        // Check if anything changed
        bool changed = false;
        
        if (!cachedMetadata_ || 
            cachedMetadata_->serialNumber != metadata.serialNumber ||
            cachedMetadata_->platform != metadata.platform ||
            cachedMetadata_->hostname != metadata.hostname)
        {
            changed = true;
            cachedMetadata_ = metadata;
        }
        
        if (!cachedState_ || 
            cachedState_->powerState != state.powerState)
        {
            changed = true;
            cachedState_ = state;
        }
        
        if (changed)
        {
            LOG_INFO("Detected changes, updating D-Bus objects...");

            // Build new model (without FRU/platform.json - those don't change at runtime)
            InventoryModel newModel = InventoryModelBuilder::build(
                std::nullopt,  // FRU doesn't change
                metadata,
                std::nullopt,  // platform.json doesn't change
                state
            );

            // Update D-Bus objects
            dbusExporter_->updateObjects(newModel);

            // Notify callback
            if (updateCallback_)
            {
                updateCallback_();
            }
        }
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("Update error: %s", e.what());
    }
}

void UpdateEngine::scheduleNextPoll()
{
    if (!running_ || pollIntervalSec_ <= 0)
    {
        return;
    }

    timer_.expires_after(std::chrono::seconds(pollIntervalSec_));
    timer_.async_wait([this](const boost::system::error_code& ec) {
        onPollTimer(ec);
    });
}

void UpdateEngine::onRedisFieldChange(const std::string& key,
                                       const std::string& field,
                                       const std::string& value)
{
    LOG_INFO("[UpdateEngine] Redis field changed: %s.%s = %s",
             key.c_str(), field.c_str(), value.c_str());

    try
    {
        bool needsUpdate = false;

        // Handle DEVICE_METADATA changes
        if (key == "DEVICE_METADATA")
        {
            // Re-read entire DEVICE_METADATA to get all fields
            auto metadata = redisAdapter_->getDeviceMetadata();

            // Check if this field actually changed from cached value
            if (!cachedMetadata_ ||
                cachedMetadata_->serialNumber != metadata.serialNumber ||
                cachedMetadata_->platform != metadata.platform ||
                cachedMetadata_->hostname != metadata.hostname)
            {
                LOG_INFO("[UpdateEngine] DEVICE_METADATA changed, updating D-Bus");
                cachedMetadata_ = metadata;
                needsUpdate = true;
            }
        }
        // Handle CHASSIS_STATE changes
        else if (key == "CHASSIS_STATE")
        {
            // Re-read entire CHASSIS_STATE to get all fields
            auto state = redisAdapter_->getChassisState();

            // Check if power state changed
            if (!cachedState_ || cachedState_->powerState != state.powerState)
            {
                LOG_INFO("[UpdateEngine] CHASSIS_STATE changed, updating D-Bus");
                cachedState_ = state;
                needsUpdate = true;
            }
        }
        // Handle HOST_STATE|switch-host changes (bmcctld -> State.Host)
        else if (key == host_state::KEY_HOST_STATE)
        {
            // The subscriber fans out one callback per hash field, so the
            // whole hash is re-read once per notification.
            if (field != host_state::FIELD_DEVICE_POWER_STATE)
            {
                return;
            }

            auto hostState = redisAdapter_->getHostState();
            if (!hostState)
            {
                LOG_WARNING("[UpdateEngine] %s not found in Redis", key.c_str());
                return;
            }

            if (!stateManager_)
            {
                LOG_WARNING("[UpdateEngine] %s changed but no StateManager attached",
                            key.c_str());
                return;
            }

            LOG_INFO("[UpdateEngine] %s changed: device_power_state=%s device_status=%s",
                     key.c_str(), hostState->devicePowerState.c_str(),
                     hostState->deviceStatus.c_str());
            stateManager_->onHostStateChanged(*hostState);
            return;
        }
        // Handle RACK_MANAGER_COMMAND|<id> changes (bmcctld command results)
        else if (key.starts_with(std::string(host_state::TABLE_RACK_MANAGER_COMMAND) + "|"))
        {
            if (field != host_state::FIELD_STATUS)
            {
                return;
            }

            std::string commandId =
                key.substr(host_state::TABLE_RACK_MANAGER_COMMAND.size() + 1);
            auto command = redisAdapter_->getRackManagerCommand(commandId);
            if (!command)
            {
                LOG_WARNING("[UpdateEngine] %s not found in Redis", key.c_str());
                return;
            }

            if (!stateManager_)
            {
                LOG_WARNING("[UpdateEngine] %s changed but no StateManager attached",
                            key.c_str());
                return;
            }

            LOG_INFO("[UpdateEngine] %s changed: command=%s status=%s result=%s",
                     key.c_str(), command->command.c_str(),
                     command->status.c_str(), command->result.c_str());
            stateManager_->onCommandStatusChanged(*command);
            return;
        }
        // Handle LIQUID_COOLING_INFO|<name> changes
        else if (key.starts_with("LIQUID_COOLING_INFO|"))
        {
            std::string sensorName =
                key.substr(std::string("LIQUID_COOLING_INFO|").size());
            LOG_INFO("[UpdateEngine] LIQUID_COOLING_INFO changed: %s",
                     sensorName.c_str());

            auto sensor = redisAdapter_->getLeakSensor(sensorName);
            if (!sensor)
            {
                LOG_WARNING("[UpdateEngine] Leak sensor %s not found in Redis",
                            sensorName.c_str());
                return;
            }

            // D-Bus objects and the ObjectMapper registry are owned by the
            // io_context thread; this callback runs on the subscriber thread.
            postLeakSensor(*sensor);
            return;
        }
        else
        {
            LOG_WARNING("[UpdateEngine] Unknown Redis key: %s", key.c_str());
            return;
        }

        // Update D-Bus objects if needed
        if (needsUpdate)
        {
            // Build new model with updated data
            InventoryModel newModel = InventoryModelBuilder::build(
                std::nullopt,      // FRU doesn't change at runtime
                cachedMetadata_,   // Updated metadata
                std::nullopt,      // platform.json doesn't change at runtime
                cachedState_       // Updated chassis state
            );

            // Update D-Bus objects
            dbusExporter_->updateObjects(newModel);

            LOG_INFO("[UpdateEngine] D-Bus objects updated successfully");

            // Notify callback
            if (updateCallback_)
            {
                updateCallback_();
            }
        }
        else
        {
            LOG_DEBUG("[UpdateEngine] No actual change detected, skipping D-Bus update");
        }
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("[UpdateEngine] Error handling Redis field change: %s", e.what());
    }
}

void UpdateEngine::syncLeakSensors()
{
    for (const auto& sensor : redisAdapter_->getLeakSensors())
    {
        postLeakSensor(sensor);
    }
}

void UpdateEngine::postLeakSensor(const LeakSensorInfo& sensor)
{
    boost::asio::post(io_, [this, sensor]() {
        std::string newState = leak_sync::trackedLeakState(sensor);

        std::optional<std::string> cached;
        if (auto it = cachedLeakStates_.find(sensor.name);
            it != cachedLeakStates_.end())
        {
            cached = it->second;
        }

        switch (leak_sync::decideLeakSensorAction(
            dbusExporter_->hasLeakSensor(sensor.name), cached, newState))
        {
            case leak_sync::LeakSensorAction::Create:
                LOG_INFO("[UpdateEngine] New leak sensor %s (%s)",
                         sensor.name.c_str(), newState.c_str());
                if (dbusExporter_->addLeakSensorObject(sensor))
                {
                    cachedLeakStates_[sensor.name] = newState;
                }
                break;
            case leak_sync::LeakSensorAction::Update:
                LOG_INFO("[UpdateEngine] Leak sensor %s state: %s -> %s",
                         sensor.name.c_str(),
                         cached ? cached->c_str() : "(none)", newState.c_str());
                cachedLeakStates_[sensor.name] = newState;
                dbusExporter_->updateLeakSensorState(sensor);
                break;
            case leak_sync::LeakSensorAction::None:
                LOG_DEBUG("[UpdateEngine] Leak sensor %s state unchanged (%s)",
                          sensor.name.c_str(), newState.c_str());
                break;
        }
    });
}

} // namespace sonic::dbus_bridge

