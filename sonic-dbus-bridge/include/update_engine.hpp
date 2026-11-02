///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2024 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// Author: Chinmoy Dey <chinmoy@nexthop.ai>
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

#pragma once

#include "types.hpp"
#include "redis_adapter.hpp"
#include "dbus_exporter.hpp"
#include "state_manager.hpp"
#include <boost/asio/steady_timer.hpp>
#include <memory>
#include <functional>

namespace sonic::dbus_bridge
{

/**
 * @brief Update engine for event-driven and periodic updates
 *
 * Supports both event-driven updates (via Redis keyspace notifications)
 * and periodic polling (as fallback). Updates D-Bus objects when changes
 * are detected.
 */
class UpdateEngine
{
  public:
    using UpdateCallback = std::function<void()>;

    /**
     * @brief Construct a new Update Engine
     *
     * @param io Boost ASIO io_context
     * @param redisAdapter Redis adapter
     * @param dbusExporter D-Bus exporter
     * @param pollIntervalSec Polling interval in seconds (0 = disable polling)
     */
    UpdateEngine(boost::asio::io_context& io,
                 std::shared_ptr<RedisAdapter> redisAdapter,
                 std::shared_ptr<DBusExporter> dbusExporter,
                 int pollIntervalSec);

    /**
     * @brief Start periodic polling (if enabled)
     */
    void start();

    /**
     * @brief Stop periodic polling
     */
    void stop();

    /**
     * @brief Set callback for update events
     *
     * Called whenever data is updated from sources
     */
    void setUpdateCallback(UpdateCallback callback)
    {
        updateCallback_ = std::move(callback);
    }

    /**
     * @brief Attach the StateManager that owns State.Host / State.Chassis
     *
     * HOST_STATE|switch-host and RACK_MANAGER_COMMAND|* changes are
     * forwarded to it.
     */
    void setStateManager(std::shared_ptr<StateManager> stateManager)
    {
        stateManager_ = std::move(stateManager);
    }

    /**
     * @brief Handle Redis field change event (event-driven)
     *
     * Called by RedisStateSubscriber when a Redis key changes.
     * Updates only the affected D-Bus properties.
     *
     * @param key Redis key that changed (e.g., "DEVICE_METADATA", "CHASSIS_STATE",
     *            "HOST_STATE|switch-host", "RACK_MANAGER_COMMAND|CMD_...")
     * @param field Redis field that changed (e.g., "serial_number", "power_state")
     * @param value New value of the field
     */
    void onRedisFieldChange(const std::string& key,
                            const std::string& field,
                            const std::string& value);

    /**
     * @brief Re-read every LIQUID_COOLING_INFO|* row and apply it
     *
     * Run on the subscriber thread once the subscriptions are active, so a
     * sensor written between the startup read and the subscription is
     * still exported.
     */
    void syncLeakSensors();

  private:
    boost::asio::io_context& io_;
    std::shared_ptr<RedisAdapter> redisAdapter_;
    std::shared_ptr<DBusExporter> dbusExporter_;
    std::shared_ptr<StateManager> stateManager_;
    int pollIntervalSec_;
    boost::asio::steady_timer timer_;
    bool running_{false};
    UpdateCallback updateCallback_;

    // Cached data for change detection
    std::optional<DeviceMetadata> cachedMetadata_;
    std::optional<ChassisState> cachedState_;
    std::map<std::string, std::string> cachedLeakStates_;

    /**
     * @brief Poll timer handler
     */
    void onPollTimer(const boost::system::error_code& ec);

    /**
     * @brief Perform update cycle
     */
    void doUpdate();

    /**
     * @brief Schedule next poll
     */
    void scheduleNextPoll();

    /**
     * @brief Apply a leak sensor snapshot on the io_context thread
     *
     * Updates the D-Bus object when the derived state changed, and creates
     * it when the sensor was not seen before.
     */
    void postLeakSensor(const LeakSensorInfo& sensor);
};

} // namespace sonic::dbus_bridge

