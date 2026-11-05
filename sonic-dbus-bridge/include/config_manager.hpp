///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2024 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

#pragma once

#include <string>
#include <vector>

namespace sonic::dbus_bridge
{

/**
 * @brief Configuration manager
 * 
 * Loads and provides access to daemon configuration
 */
class ConfigManager
{
  public:
    /**
     * @brief Load configuration from YAML file
     * 
     * @param configPath Path to config.yaml
     * @return true on success
     */
    bool load(const std::string& configPath);

    // Redis configuration
    const std::string& getConfigDbHost() const { return configDbHost_; }
    int getConfigDbPort() const { return configDbPort_; }
    int getConfigDbIndex() const { return configDbIndex_; }

    const std::string& getStateDbHost() const { return stateDbHost_; }
    int getStateDbPort() const { return stateDbPort_; }
    int getStateDbIndex() const { return stateDbIndex_; }

    // Platform configuration
    const std::string& getPlatformJsonPath() const { return platformJsonPath_; }
    const std::vector<std::string>& getFruEepromPaths() const { return fruEepromPaths_; }

    // Update configuration
    int getPollIntervalSec() const { return pollIntervalSec_; }

    // Logging configuration
    const std::string& getLogLevel() const { return logLevel_; }

    // D-Bus configuration
    const std::string& getDbusServiceName() const { return dbusServiceName_; }

  private:
    // Redis
    std::string configDbHost_{}; // Empty selects the configured local socket.
    int configDbPort_{6379};
    int configDbIndex_{4};

    std::string stateDbHost_{}; // Explicit nonempty hosts retain TCP.
    int stateDbPort_{6379};
    int stateDbIndex_{6};

    // Platform
    std::string platformJsonPath_{"/usr/share/sonic/device/${PLATFORM}/platform.json"};
    std::vector<std::string> fruEepromPaths_{
        "/sys/bus/i2c/devices/0-0050/eeprom",
        "/sys/bus/i2c/devices/1-0051/eeprom"
    };

    // Update
    int pollIntervalSec_{30};

    // Logging
    std::string logLevel_{"INFO"};

    // D-Bus
    std::string dbusServiceName_{"xyz.openbmc_project.Inventory.Manager"};
};

} // namespace sonic::dbus_bridge

