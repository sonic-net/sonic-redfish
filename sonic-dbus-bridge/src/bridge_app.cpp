///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2024 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

#include "bridge_app.hpp"
#include "inventory_model.hpp"
#include "logger.hpp"
#include "config.h"
#include <systemd/sd-bus.h>
#include <signal.h>

namespace sonic::dbus_bridge
{

// Signal handlers for runtime log file dumping
static void handleLogSignal(int signum)
{
    if (signum == SIGUSR1)
    {
        // Enable file logging
        LOGGER_ENABLE_FILE_LOGGING();
    }
    else if (signum == SIGUSR2)
    {
        // Disable file logging
        LOGGER_DISABLE_FILE_LOGGING();
    }
}

BridgeApp::BridgeApp(const std::string& configPath)
    : configPath_(configPath), signals_(io_, SIGINT, SIGTERM)
{
    // Initialize logger from environment variable
    LOGGER_INIT();

    // Register signal handlers for runtime log file dumping
    // SIGUSR1: start dumping logs to /tmp/sonic-dbus-bridge.log
    // SIGUSR2: stop dumping logs and delete the file
    signal(SIGUSR1, handleLogSignal);
    signal(SIGUSR2, handleLogSignal);
}

bool BridgeApp::initialize()
{
    LOG_INFO("Initializing SONiC D-Bus Bridge...");

    // Load configuration
    if (!loadConfiguration())
    {
        LOG_ERROR("Failed to load configuration");
        return false;
    }

    // Connect to D-Bus and claim all service names
    if (!connectDbus())
    {
        LOG_ERROR("Failed to connect to D-Bus");
        return false;
    }

    // Initialize ObjectMapper service for bmcweb discovery (uses mapper connection)
    objectMapper_ = std::make_unique<ObjectMapperService>(*mapperServer_);
    if (!objectMapper_->initialize())
    {
        LOG_ERROR("Failed to initialize ObjectMapper service");
        return false;
    }

    // Initialize data sources
    initializeDataSources();

    // Build initial inventory model
    currentModel_ = buildInitialModel();

    // Create D-Bus objects (inventory)
    createDbusObjects();

    // Create state objects
    createStateObjects();

    // Initialize user management (non-fatal if it fails)
    initializeUserManager();

    // Start update engine
    startUpdateEngine();

    // Setup signal handlers
    signals_.async_wait([this](const boost::system::error_code& ec, int signal) {
        handleSignal(ec, signal);
    });

    logHealthReport();

    return true;
}

int BridgeApp::run()
{
    LOG_INFO("Running main event loop...");

    try
    {
        // sdbusplus::asio::connection handles D-Bus event integration automatically
        io_.run();
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("Event loop error: %s", e.what());
        return 1;
    }

    LOG_INFO("Exiting...");
    return 0;
}

void BridgeApp::shutdown()
{
    LOG_INFO("Shutting down...");

    if (redisSubscriber_)
    {
        LOG_INFO("Stopping Redis subscriber...");
        redisSubscriber_->stop();
    }

    if (updateEngine_)
    {
        updateEngine_->stop();
    }

    // sdbusplus connection cleanup is automatic (RAII)
    io_.stop();
}

bool BridgeApp::loadConfiguration()
{
    configMgr_ = std::make_unique<ConfigManager>();
    return configMgr_->load(configPath_);
}

bool BridgeApp::connectDbus()
{
    try
    {
        sd_bus* bus = nullptr;
        int r;

        // Inventory Manager connection
        LOG_INFO("Requesting D-Bus name: %s", INVENTORY_MANAGER_BUSNAME);
        r = sd_bus_open_system(&bus);
        if (r < 0)
        {
            LOG_ERROR("Failed to open system bus for Inventory: %s", strerror(-r));
            return false;
        }
        inventoryConn_ = std::make_shared<sdbusplus::asio::connection>(io_, bus);
        inventoryConn_->request_name(INVENTORY_MANAGER_BUSNAME);
        inventoryServer_ = std::make_unique<sdbusplus::asio::object_server>(inventoryConn_);

        // ObjectMapper connection
        LOG_INFO("Requesting D-Bus name: %s", OBJECT_MAPPER_BUSNAME);
        bus = nullptr;
        r = sd_bus_open_system(&bus);
        if (r < 0)
        {
            LOG_ERROR("Failed to open system bus for ObjectMapper: %s", strerror(-r));
            return false;
        }
        mapperConn_ = std::make_shared<sdbusplus::asio::connection>(io_, bus);
        mapperConn_->request_name(OBJECT_MAPPER_BUSNAME);
        mapperServer_ = std::make_unique<sdbusplus::asio::object_server>(mapperConn_);

        // User Manager connection
        LOG_INFO("Requesting D-Bus name: %s", USER_MANAGER_BUSNAME);
        bus = nullptr;
        r = sd_bus_open_system(&bus);
        if (r < 0)
        {
            LOG_ERROR("Failed to open system bus for User Manager: %s", strerror(-r));
            return false;
        }
        userConn_ = std::make_shared<sdbusplus::asio::connection>(io_, bus);
        userConn_->request_name(USER_MANAGER_BUSNAME);
        userServer_ = std::make_unique<sdbusplus::asio::object_server>(userConn_);

        LOG_INFO("Connected to D-Bus successfully (2 connections)");

        // State.Host connection
        LOG_INFO("Requesting D-Bus name: %s", STATE_HOST_BUSNAME);
        bus = nullptr;
        r = sd_bus_open_system(&bus);
        if (r < 0)
        {
            LOG_ERROR("Failed to open system bus for State.Host: %s", strerror(-r));
            return false;
        }
        stateConn_ = std::make_shared<sdbusplus::asio::connection>(io_, bus);
        stateConn_->request_name(STATE_HOST_BUSNAME);
        stateServer_ = std::make_unique<sdbusplus::asio::object_server>(stateConn_);

        LOG_INFO("Connected to D-Bus successfully (5 connections)");
        return true;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("Failed to connect to D-Bus: %s", e.what());
        return false;
    }
}

void BridgeApp::initializeDataSources()
{
    LOG_INFO("Initializing data sources...");
    
    // Initialize Redis adapter
    redisAdapter_ = std::make_shared<RedisAdapter>(
        configMgr_->getConfigDbHost(),
        configMgr_->getConfigDbPort(),
        configMgr_->getStateDbHost(),
        configMgr_->getStateDbPort()
    );
    
    if (redisAdapter_->connect())
    {
        updateHealth(DataSource::RedisConfigDb, 
                    redisAdapter_->isConfigDbConnected() ? 
                        DataSourceHealth::Healthy : DataSourceHealth::Unavailable);
        updateHealth(DataSource::RedisStateDb,
                    redisAdapter_->isStateDbConnected() ?
                        DataSourceHealth::Healthy : DataSourceHealth::Unavailable);
    }
    else
    {
        updateHealth(DataSource::RedisConfigDb, DataSourceHealth::Unavailable);
        updateHealth(DataSource::RedisStateDb, DataSourceHealth::Unavailable);
    }
    
    // Initialize platform.json adapter
    platformAdapter_ = std::make_unique<PlatformJsonAdapter>(
        configMgr_->getPlatformJsonPath()
    );
    
    if (platformAdapter_->load())
    {
        updateHealth(DataSource::PlatformJson, DataSourceHealth::Healthy);
    }
    else
    {
        updateHealth(DataSource::PlatformJson, DataSourceHealth::Unavailable);
    }
    
    // Initialize FRU adapter
    fruAdapter_ = std::make_unique<FruAdapter>(
        configMgr_->getFruEepromPaths()
    );
    
    if (fruAdapter_->scanAndLoad())
    {
        updateHealth(DataSource::FruEeprom, DataSourceHealth::Healthy);
    }
    else
    {
        updateHealth(DataSource::FruEeprom, DataSourceHealth::Unavailable);
    }
}

InventoryModel BridgeApp::buildInitialModel()
{
    LOG_INFO("Building initial inventory model...");

    std::optional<FruInfo> fruInfo;
    if (fruAdapter_->isLoaded())
    {
        fruInfo = fruAdapter_->getFruInfo();
    }

    std::optional<DeviceMetadata> deviceMetadata;
    if (redisAdapter_->isConfigDbConnected())
    {
        deviceMetadata = redisAdapter_->getDeviceMetadata();
    }

    std::optional<PlatformDescription> platformDesc;
    if (platformAdapter_->isLoaded())
    {
        platformDesc = platformAdapter_->getPlatformDescription();
    }

    std::optional<ChassisState> chassisState;
    if (redisAdapter_->isStateDbConnected())
    {
        chassisState = redisAdapter_->getChassisState();
    }

    auto model = InventoryModelBuilder::build(fruInfo, deviceMetadata, platformDesc, chassisState);

    // Read firmware versions for FirmwareInventory
    model.firmwareVersions = redisAdapter_->getFirmwareVersions();

    // Read leak sensors from STATE_DB
    if (redisAdapter_->isStateDbConnected())
    {
        model.leakSensors = redisAdapter_->getLeakSensors();
    }

    return model;
}

void BridgeApp::createDbusObjects()
{
    // Use inventory connection for inventory/state objects
    dbusExporter_ = std::make_shared<DBusExporter>(*inventoryServer_);
    if (!dbusExporter_->createObjects(currentModel_))
    {
        LOG_WARNING("Failed to create some D-Bus objects");
    }

    // Register the objects we own with the local ObjectMapper so that
    // bmcweb can discover them using GetSubTree* calls.
    if (objectMapper_)
    {
        // Chassis inventory object
        objectMapper_->registerObject(
            "/xyz/openbmc_project/inventory/system/chassis",
            {"xyz.openbmc_project.Inventory.Item.Chassis",
             "xyz.openbmc_project.Inventory.Decorator.Asset",
             "xyz.openbmc_project.Inventory.Decorator.Model"});

        // System inventory object (no Item.System interface yet,
        // only common decorators)
        objectMapper_->registerObject(
            "/xyz/openbmc_project/inventory/system/system0",
            {"xyz.openbmc_project.Inventory.Decorator.Asset",
             "xyz.openbmc_project.Inventory.Decorator.Model"});

        // Chassis state object
        objectMapper_->registerObject(
            "/xyz/openbmc_project/state/chassis0",
            {"xyz.openbmc_project.State.Chassis"});

        // Firmware inventory objects (for /redfish/v1/UpdateService/FirmwareInventory)
        for (const auto& fw : currentModel_.firmwareVersions)
        {
            std::string fwPath = "/xyz/openbmc_project/software/" + fw.id;
            objectMapper_->registerObject(
                fwPath,
                {"xyz.openbmc_project.Software.Version",
                 "xyz.openbmc_project.Software.Activation"});
        }

        // Leak sensor objects
        for (const auto& sensor : currentModel_.leakSensors)
        {
            std::string sensorPath = std::string("/xyz/openbmc_project/sensors/leak/") + sensor.name;
            objectMapper_->registerObject(
                sensorPath,
                {"xyz.openbmc_project.Inventory.Item.LeakDetector",
                 "xyz.openbmc_project.State.Decorator.OperationalStatus",
                 "xyz.openbmc_project.Inventory.Item"});
        }
    }
}

void BridgeApp::createStateObjects()
{
    // Use dedicated State.Host connection for state objects
    stateManager_ = std::make_unique<StateManager>(*stateServer_, io_);

    if (!stateManager_->createStateObjects())
    {
        LOG_WARNING("Failed to create state objects");
        return;
    }

    // Register the state object with the local ObjectMapper so that
    // bmcweb can discover it using GetSubTree* calls.
    if (objectMapper_)
    {
        objectMapper_->registerObject(
            "/xyz/openbmc_project/state/host0",
            {"xyz.openbmc_project.State.Host"});
    }
}

void BridgeApp::startUpdateEngine()
{
    // Create UpdateEngine with polling disabled - event-driven mode only
    updateEngine_ = std::make_unique<UpdateEngine>(
        io_,
        redisAdapter_,
        dbusExporter_,
        0  // Disable polling - use event-driven updates only
    );

    updateEngine_->setUpdateCallback([this]() {
        LOG_INFO("Inventory updated");
    });

    updateEngine_->start();

    // Start event-driven Redis subscriber for multiple keys
    LOG_INFO("Starting event-driven Redis subscriber...");

    redisSubscriber_ = std::make_unique<RedisStateSubscriber>();

    // Subscribe to multiple Redis keys for event-driven updates
    std::vector<std::string> keysToSubscribe = {
        "DEVICE_METADATA",   // Serial number, platform, hostname
        "CHASSIS_STATE",     // Power state
        "SWITCH_HOST_STATE"  // Host state (for future use)
    };

    // Dynamically add leak sensor keys discovered at startup
    for (const auto& sensor : currentModel_.leakSensors)
    {
        keysToSubscribe.push_back("LEAK_SENSOR|" + sensor.name);
        LOG_INFO("Subscribing to leak sensor key: LEAK_SENSOR|%s",
                 sensor.name.c_str());
    }

    // Register callback to UpdateEngine
    auto callback = [this](const std::string& key,
                          const std::string& field,
                          const std::string& value) {
        // Forward Redis events to UpdateEngine
        updateEngine_->onRedisFieldChange(key, field, value);
    };

    // Start subscriber with multiple keys (use STATE_DB connection)
    bool started = redisSubscriber_->startMultiKey(
        configMgr_->getStateDbHost(),
        configMgr_->getStateDbPort(),
        keysToSubscribe,
        callback
    );

    if (!started)
    {
        LOG_ERROR("FATAL: Failed to start event-driven Redis subscriber");
        LOG_ERROR("Cannot continue without event-driven updates");
        throw std::runtime_error("Failed to start event-driven Redis subscriber");
    }

    LOG_INFO("Event-driven Redis subscriber started successfully");
    LOG_INFO("Subscribed to %zu Redis keys for instant updates", keysToSubscribe.size());
}

void BridgeApp::handleSignal(const boost::system::error_code& ec, int signal)
{
    if (!ec)
    {
        LOG_INFO("Received signal %d", signal);
        shutdown();
    }
}

void BridgeApp::updateHealth(DataSource source, DataSourceHealth health)
{
    healthStatus_[source] = health;
}

void BridgeApp::logHealthReport()
{
    LOG_INFO("=== Data Source Health Report ===");

    auto printHealth = [](const std::string& name, DataSourceHealth health) {
        const char* status = "Unknown";
        switch (health)
        {
            case DataSourceHealth::Healthy:
                status = "Healthy";
                LOG_INFO("  %s: %s", name.c_str(), status);
                break;
            case DataSourceHealth::Degraded:
                status = "Degraded";
                LOG_WARNING("  %s: %s", name.c_str(), status);
                break;
            case DataSourceHealth::Unavailable:
                status = "Unavailable";
                LOG_WARNING("  %s: %s", name.c_str(), status);
                break;
        }
    };

    printHealth("CONFIG_DB", healthStatus_[DataSource::RedisConfigDb]);
    printHealth("STATE_DB", healthStatus_[DataSource::RedisStateDb]);
    printHealth("platform.json", healthStatus_[DataSource::PlatformJson]);
    printHealth("FRU EEPROM", healthStatus_[DataSource::FruEeprom]);

    LOG_INFO("==================================");
}

void BridgeApp::initializeUserManager()
{
    LOG_INFO("Initializing user management...");

    try
    {
        // Create user manager using user connection (separate from inventory/mapper)
        // (scans /etc/passwd on construction)
        userMgr_ = std::make_unique<sonic::user::UserMgr>(
            *userServer_, "/xyz/openbmc_project/user", objectMapper_.get());

	        // Register user manager with ObjectMapper for bmcweb discovery
        if (objectMapper_)
        {
	            objectMapper_->registerObject(
	                "/xyz/openbmc_project/user",
	                {USER_MANAGER_BUSNAME},
	                USER_MANAGER_BUSNAME);

	            // Register each existing user object with User.Manager service name.
	            // User objects are read-only; creation/deletion is handled outside
	            // of sonic-dbus-bridge.
	            for (const auto& [username, userObj] : userMgr_->getUsers())
	            {
	                std::string userPath = "/xyz/openbmc_project/user/" + username;
	                objectMapper_->registerObject(
	                    userPath,
	                    {"xyz.openbmc_project.User.Attributes"},
	                    USER_MANAGER_BUSNAME);
	            }
        }

        LOG_INFO("User management initialized successfully");
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("Failed to initialize user management: %s", e.what());
        LOG_WARNING("User management not available");
        userMgr_.reset();
    }
}

} // namespace sonic::dbus_bridge

