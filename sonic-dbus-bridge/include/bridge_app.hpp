///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2026 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

#pragma once

#include "types.hpp"
#include "redis_adapter.hpp"
#include "platform_json_adapter.hpp"
#include "fru_adapter.hpp"
#include "dbus_exporter.hpp"
#include "update_engine.hpp"
#include "config_manager.hpp"
#include "object_mapper.hpp"
#include "user_mgr.hpp"
#include "state_manager.hpp"
#include "rack_manager_receiver.hpp"
#include "redis_state_subscriber.hpp"
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <memory>
#include <map>

namespace sonic::dbus_bridge
{

    /**
     * @brief Main bridge application
     *
     * Coordinates all components:
     * - Initializes data sources (Redis, platform.json, FRU)
     * - Builds initial inventory model
     * - Creates D-Bus objects
     * - Runs event loop with periodic updates
     */
    class BridgeApp
    {
        public:
            /**
             * @brief Construct a new Bridge App
             *
             * @param configPath Path to configuration file
             */
            explicit BridgeApp(const std::string& configPath);

            /**
             * @brief Initialize the application
             *
             * - Load configuration
             * - Connect to data sources
             * - Build initial inventory
             * - Create D-Bus objects
             *
             * @return true on success
             * @return false on fatal error
             */
            bool initialize();

            /**
             * @brief Run the application
             *
             * Enters main event loop (blocking)
             *
             * @return Exit code (0 = success)
             */
            int run();

            /**
             * @brief Shutdown the application
             */
            void shutdown();

        private:
            std::string configPath_;
            std::unique_ptr<ConfigManager> configMgr_;

            // Boost ASIO
            boost::asio::io_context io_;
            boost::asio::signal_set signals_;

            // D-Bus connections (one per service for proper object separation)
            // Each service has its own connection so busctl tree shows only its objects
            std::shared_ptr<sdbusplus::asio::connection> inventoryConn_;
            std::unique_ptr<sdbusplus::asio::object_server> inventoryServer_;

            std::shared_ptr<sdbusplus::asio::connection> mapperConn_;
            std::unique_ptr<sdbusplus::asio::object_server> mapperServer_;

            std::shared_ptr<sdbusplus::asio::connection> userConn_;
            std::unique_ptr<sdbusplus::asio::object_server> userServer_;

            std::shared_ptr<sdbusplus::asio::connection> stateConn_;
            std::unique_ptr<sdbusplus::asio::object_server> stateServer_;

            // Data source adapters
            std::shared_ptr<RedisAdapter> redisAdapter_;
            std::unique_ptr<PlatformJsonAdapter> platformAdapter_;
            std::unique_ptr<FruAdapter> fruAdapter_;

            // Core components
            std::shared_ptr<DBusExporter> dbusExporter_;
            std::unique_ptr<UpdateEngine> updateEngine_;
            std::unique_ptr<ObjectMapperService> objectMapper_;

            // User management
            std::unique_ptr<sonic::user::UserMgr> userMgr_;

            // State management
            std::unique_ptr<StateManager> stateManager_;

            // Event-driven Redis subscriber
            std::unique_ptr<RedisStateSubscriber> redisSubscriber_;

            // Rack manager alert / telemetry receiver
            std::unique_ptr<RackManagerReceiver> rackManagerReceiver_;

            // Current inventory model
            InventoryModel currentModel_;

            // Health tracking
            std::map<DataSource, DataSourceHealth> healthStatus_;

            /**
             * @brief Load configuration file
             */
            bool loadConfiguration();

            /**
             * @brief Connect to D-Bus system bus
             */
            bool connectDbus();

            /**
             * @brief Initialize data sources
             */
            void initializeDataSources();

            /**
             * @brief Build initial inventory model
             */
            InventoryModel buildInitialModel();

            /**
             * @brief Create D-Bus objects
             */
            void createDbusObjects();

            /**
             * @brief Create state objects
             */
            void createStateObjects();

            /**
             * @brief Start update engine
             */
            void startUpdateEngine();

            /**
             * @brief Handle signals (SIGTERM, SIGINT)
             */
            void handleSignal(const boost::system::error_code& ec, int signal);

            /**
             * @brief Update health status
             */
            void updateHealth(DataSource source, DataSourceHealth health);

            /**
             * @brief Log health report
             */
            void logHealthReport();

            /**
             * @brief Initialize user management subsystem
             *
             * Creates UserMgr instance using the shared object server.
             * Non-fatal if it fails - bridge continues without user management.
             */
            void initializeUserManager();
    };

} // namespace sonic::dbus_bridge



