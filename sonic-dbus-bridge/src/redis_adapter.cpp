///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2024 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// Author: Chinmoy Dey <chinmoy@nexthop.ai>
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

#include "redis_adapter.hpp"
#include "host_state_mapping.hpp"
#include "logger.hpp"
#include <cerrno>
#include <cstring>
#include <fstream>

namespace sonic::dbus_bridge
{

RedisAdapter::RedisAdapter(const std::string& configDbHost, int configDbPort,
                           const std::string& stateDbHost, int stateDbPort)
    : configDbHost_(configDbHost), configDbPort_(configDbPort),
      stateDbHost_(stateDbHost), stateDbPort_(stateDbPort)
{
}

RedisAdapter::~RedisAdapter()
{
    if (configDbContext_)
    {
        redisFree(configDbContext_);
    }
    if (stateDbContext_)
    {
        redisFree(stateDbContext_);
    }
}

bool RedisAdapter::connect()
{
    LOG_INFO("Connecting to Redis databases...");

    // Connect to CONFIG_DB (DB 4)
    configDbContext_ = connectToDb(configDbHost_, configDbPort_, 4);
    if (configDbContext_)
    {
        LOG_INFO("Connected to CONFIG_DB");
    }
    else
    {
        LOG_WARNING("Failed to connect to CONFIG_DB");
    }

    // Connect to STATE_DB (DB 6)
    stateDbContext_ = connectToDb(stateDbHost_, stateDbPort_, 6);
    if (stateDbContext_)
    {
        LOG_INFO("Connected to STATE_DB");
    }
    else
    {
        LOG_WARNING("Failed to connect to STATE_DB");
    }

    // Return true if at least one connection succeeded
    return (configDbContext_ != nullptr) || (stateDbContext_ != nullptr);
}

void RedisAdapter::noteReadResult(bool ok, const char* command,
                                  const std::string& key)
{
    if (ok)
    {
        if (readFailureLogged_)
        {
            LOG_NOTICE("Redis reads are working again");
            readFailureLogged_ = false;
        }
        return;
    }
    if (!readFailureLogged_)
    {
        LOG_WARNING("Redis read failed: %s %s (further failures not logged "
                    "until reads recover)",
                    command, key.c_str());
        readFailureLogged_ = true;
    }
}

redisContext* RedisAdapter::connectToDb(const std::string& host, int port,
                                        int dbIndex, bool logFailure)
{
    struct timeval timeout = {2, 0}; // 2 seconds
    redisContext* ctx = nullptr;
    bool connected = false;

    // Try TCP connection first (most reliable for SONiC)
    LOG_DEBUG("Attempting TCP connection to %s:%d...", host.c_str(), port);
    ctx = redisConnectWithTimeout(host.c_str(), port, timeout);

    if (!ctx)
    {
        LOG_ERROR("TCP: Failed to allocate Redis context (out of memory?)");
    }
    else if (ctx->err)
    {
        LOG_DEBUG("TCP: Connection failed: %s (errno: %d)", ctx->errstr, ctx->err);
        redisFree(ctx);
        ctx = nullptr;
    }
    else
    {
        LOG_INFO("Connected to Redis via TCP: %s:%d", host.c_str(), port);
        connected = true;
    }

    // If TCP failed, try Unix socket as fallback
    if (!connected)
    {
        const char* unixSockets[] = {
            "/var/run/redis/redis.sock",
            "/run/redis/redis.sock",
            "/var/run/redis.sock",
            nullptr
        };

        for (int i = 0; unixSockets[i] != nullptr && !connected; i++)
        {
            LOG_DEBUG("Attempting Unix socket connection to %s...", unixSockets[i]);
            ctx = redisConnectUnixWithTimeout(unixSockets[i], timeout);

            if (!ctx)
            {
                LOG_ERROR("Unix socket: Failed to allocate Redis context");
            }
            else if (ctx->err)
            {
                LOG_DEBUG("Unix socket: Connection failed: %s (errno: %d)", ctx->errstr, ctx->err);
                redisFree(ctx);
                ctx = nullptr;
            }
            else
            {
                LOG_INFO("Connected to Redis via Unix socket: %s", unixSockets[i]);
                connected = true;
            }
        }
    }

    if (!connected || !ctx)
    {
        if (logFailure)
        {
            LOG_ERROR("All Redis connection attempts failed");
        }
        return nullptr;
    }

    // Select database
    LOG_DEBUG("Selecting Redis database %d...", dbIndex);
    redisReply* reply = static_cast<redisReply*>(
        redisCommand(ctx, "SELECT %d", dbIndex));

    if (!reply)
    {
        LOG_ERROR("Failed to send SELECT command (connection lost?)");
        redisFree(ctx);
        return nullptr;
    }

    if (reply->type == REDIS_REPLY_ERROR)
    {
        LOG_ERROR("Failed to select DB %d: %s", dbIndex, reply->str);
        freeReplyObject(reply);
        redisFree(ctx);
        return nullptr;
    }

    freeReplyObject(reply);
    LOG_DEBUG("Selected database %d", dbIndex);
    return ctx;
}

DeviceMetadata RedisAdapter::getDeviceMetadata()
{
    DeviceMetadata metadata;
    
    if (!configDbContext_)
    {
        LOG_WARNING("DEVICE_METADATA not read: CONFIG_DB is not connected");
        return metadata; // Return empty metadata
    }

    // Read DEVICE_METADATA|localhost hash
    auto fields = hgetall(configDbContext_, "DEVICE_METADATA|localhost");

    if (!fields.empty())
    {
        if (fields.count("platform")) metadata.platform = fields["platform"];
        if (fields.count("hwsku")) metadata.hwsku = fields["hwsku"];
        if (fields.count("hostname")) metadata.hostname = fields["hostname"];
        if (fields.count("mac")) metadata.mac = fields["mac"];
        if (fields.count("type")) metadata.type = fields["type"];
        if (fields.count("manufacturer")) metadata.manufacturer = fields["manufacturer"];
        if (fields.count("serial_number")) metadata.serialNumber = fields["serial_number"];
        if (fields.count("part_number")) metadata.partNumber = fields["part_number"];
        if (fields.count("model")) metadata.model = fields["model"];
    }

    return metadata;
}

ChassisState RedisAdapter::getChassisState()
{
    ChassisState state;
    
    if (!stateDbContext_)
    {
        LOG_WARNING("CHASSIS_STATE not read: STATE_DB is not connected; "
                    "using the default power state");
        return state; // Return default state (on)
    }

    // Try to read chassis state from STATE_DB
    auto powerState = hget(stateDbContext_, "CHASSIS_STATE|chassis0", "power_state");
    if (powerState)
    {
        state.powerState = *powerState;
    }
    
    return state;
}

std::optional<HostStateInfo> RedisAdapter::getHostState()
{
    if (!stateDbContext_)
    {
        return std::nullopt;
    }

    auto fields = hgetall(stateDbContext_,
                          std::string(host_state::KEY_HOST_STATE));
    if (fields.empty())
    {
        return std::nullopt;
    }

    HostStateInfo info;
    info.devicePowerState = fields[std::string(host_state::FIELD_DEVICE_POWER_STATE)];
    info.deviceStatus = fields[std::string(host_state::FIELD_DEVICE_STATUS)];
    info.lastChangeTimestamp = fields["last_change_timestamp"];
    return info;
}

std::optional<RackManagerCommandInfo> RedisAdapter::getRackManagerCommand(
    const std::string& commandId)
{
    if (!stateDbContext_ || commandId.empty())
    {
        return std::nullopt;
    }

    std::string key =
        std::string(host_state::TABLE_RACK_MANAGER_COMMAND) + "|" + commandId;
    auto fields = hgetall(stateDbContext_, key);
    if (fields.empty())
    {
        return std::nullopt;
    }

    RackManagerCommandInfo info;
    info.commandId = commandId;
    info.command = fields[std::string(host_state::FIELD_COMMAND)];
    info.status = fields[std::string(host_state::FIELD_STATUS)];
    info.result = fields[std::string(host_state::FIELD_RESULT)];
    info.lastChangeTimestamp = fields["last_change_timestamp"];
    return info;
}

std::map<std::string, std::string> RedisAdapter::hgetall(redisContext* ctx,
                                                          const std::string& key)
{
    std::map<std::string, std::string> result;
    
    redisReply* reply = static_cast<redisReply*>(
        redisCommand(ctx, "HGETALL %s", key.c_str()));
    
    if (!reply)
    {
        noteReadResult(false, "HGETALL", key);
        return result;
    }
    
    if (reply->type == REDIS_REPLY_ARRAY && reply->elements > 0)
    {
        for (size_t i = 0; i < reply->elements; i += 2)
        {
            if (i + 1 < reply->elements)
            {
                std::string field(reply->element[i]->str, reply->element[i]->len);
                std::string value(reply->element[i+1]->str, reply->element[i+1]->len);
                result[field] = value;
            }
        }
    }
    
    if (reply->type == REDIS_REPLY_ERROR)
    {
        // An error reply (WRONGTYPE, LOADING) is a failed read, not an
        // empty hash.
        LOG_WARNING("Redis HGETALL %s answered with an error: %s",
                    key.c_str(), reply->str);
        freeReplyObject(reply);
        noteReadResult(false, "HGETALL", key);
        return result;
    }

    freeReplyObject(reply);
    noteReadResult(true, "HGETALL", key);
    // Field count only: values may hold data that must not be logged.
    LOG_DEBUG("Redis read ok: HGETALL %s (%zu fields)", key.c_str(),
              result.size());
    return result;
}

std::optional<std::string> RedisAdapter::getRedfishClientCnames()
{
    // Not read through hget(), which cannot distinguish a field that is not
    // set from a read that failed. That difference decides whether client
    // certificates are accepted, so it must not be lost.
    return readConfigField("REDFISH|certs", "client_crt_cname", true);
}

std::optional<std::string> RedisAdapter::getLogLevel()
{
    // The caller reports a failure itself, and retries.
    return readConfigField("LOGGER|sonic-dbus-bridge", "LOGLEVEL", false);
}

redisContext* RedisAdapter::openConfigDbConnection()
{
    return connectToDb(configDbHost_, configDbPort_, 4, false);
}

std::optional<std::string> RedisAdapter::readConfigField(const char* key,
                                                         const char* field,
                                                         bool logFailure)
{
    if (!configDbContext_)
    {
        // Not connected at startup, or dropped after an I/O error below.
        configDbContext_ =
            connectToDb(configDbHost_, configDbPort_, 4, logFailure);
        if (!configDbContext_)
        {
            return std::nullopt;
        }
    }

    redisReply* reply = static_cast<redisReply*>(
        redisCommand(configDbContext_, "HGET %s %s", key, field));
    if (!reply)
    {
        // hiredis leaves the context unusable after an I/O error, so drop it
        // and reconnect on the next read.
        if (logFailure)
        {
            LOG_WARNING("CONFIG_DB read of %s failed: %s", field,
                        configDbContext_->errstr);
        }
        redisFree(configDbContext_);
        configDbContext_ = nullptr;
        return std::nullopt;
    }

    std::optional<std::string> result;
    if (reply->type == REDIS_REPLY_STRING)
    {
        result = std::string(reply->str, reply->len);
    }
    else if (reply->type == REDIS_REPLY_NIL)
    {
        result = std::string(); // readable, field not set
    }
    else if (logFailure)
    {
        // An error reply (e.g. LOADING while Redis starts) is a failed read,
        // not an unset field.
        LOG_WARNING("CONFIG_DB read of %s failed: %s", field,
                    reply->type == REDIS_REPLY_ERROR ? reply->str
                                                     : "unexpected reply type");
    }

    freeReplyObject(reply);
    return result;
}

std::optional<std::string> RedisAdapter::hget(redisContext* ctx,
                                               const std::string& key,
                                               const std::string& field)
{
    redisReply* reply = static_cast<redisReply*>(
        redisCommand(ctx, "HGET %s %s", key.c_str(), field.c_str()));
    
    if (!reply)
    {
        noteReadResult(false, "HGET", key);
        return std::nullopt;
    }
    
    std::optional<std::string> result;
    if (reply->type == REDIS_REPLY_STRING)
    {
        result = std::string(reply->str, reply->len);
    }
    else if (reply->type == REDIS_REPLY_ERROR)
    {
        // An error reply is a failed read, not an absent field.
        LOG_WARNING("Redis HGET %s %s answered with an error: %s",
                    key.c_str(), field.c_str(), reply->str);
        freeReplyObject(reply);
        noteReadResult(false, "HGET", key);
        return std::nullopt;
    }
    
    freeReplyObject(reply);
    noteReadResult(true, "HGET", key);
    LOG_DEBUG("Redis read ok: HGET %s %s (%s)", key.c_str(), field.c_str(),
              result ? "found" : "absent");
    return result;
}

static std::string readSonicVersionField(const std::string& field)
{
    std::ifstream f("/etc/sonic/sonic_version.yml");
    if (!f.is_open())
    {
        LOG_DEBUG("Cannot open /etc/sonic/sonic_version.yml: %s",
                  strerror(errno));
        return "";
    }
    std::string line;
    while (std::getline(f, line))
    {
        auto pos = line.find(':');
        if (pos == std::string::npos)
        {
            continue;
        }
        std::string key = line.substr(0, pos);
        // Trim whitespace from key
        while (!key.empty() && (key.front() == ' ' || key.front() == '\t'))
            key.erase(key.begin());
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
            key.pop_back();

        if (key != field)
        {
            continue;
        }

        std::string val = line.substr(pos + 1);
        // Trim whitespace and quotes
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t' || val.front() == '\''))
            val.erase(val.begin());
        while (!val.empty() && (val.back() == ' ' || val.back() == '\t' || val.back() == '\'' || val.back() == '\r'))
            val.pop_back();
        return val;
    }
    return "";
}

std::vector<FirmwareVersionInfo> RedisAdapter::getFirmwareVersions()
{
    std::vector<FirmwareVersionInfo> versions;

    // 1. SONiC OS version — from switch's Redis STATE_DB
    {
        std::string sonicVersion = "N/A";
        if (stateDbContext_)
        {
            auto ver = hget(stateDbContext_, "BMC_FW_INVENTORY|SONIC_OS", "version");
            if (ver && !ver->empty())
            {
                sonicVersion = *ver;
            }
            else
            {
                LOG_WARNING("FirmwareInventory: switch not found in STATE_DB");
            }
        }
        FirmwareVersionInfo fw;
        fw.id = "switch";
        fw.version = sonicVersion;
        fw.purpose = FirmwarePurpose::Host;
        versions.push_back(fw);
        LOG_INFO("FirmwareInventory: switch = %s", sonicVersion.c_str());
    }

    // 2. BMC firmware version — local to BMC, read from /etc/sonic/sonic_version.yml
    {
        std::string bmcVer = readSonicVersionField("build_version");
        if (bmcVer.empty())
        {
            bmcVer = "N/A";
            LOG_WARNING("FirmwareInventory: BMC version not found in /etc/sonic/sonic_version.yml");
        }
        FirmwareVersionInfo fw;
        fw.id = "bmc";
        fw.version = bmcVer;
        fw.purpose = FirmwarePurpose::BMC;
        versions.push_back(fw);
        LOG_INFO("FirmwareInventory: bmc = %s", bmcVer.c_str());
    }

    // 3. BIOS version — from switch's Redis STATE_DB
    {
        std::string biosVer = "N/A";
        if (stateDbContext_)
        {
            auto ver = hget(stateDbContext_, "BMC_FW_INVENTORY|BIOS", "version");
            if (ver && !ver->empty())
            {
                biosVer = *ver;
            }
            else
            {
                LOG_WARNING("FirmwareInventory: bios not found in STATE_DB");
            }
        }
        FirmwareVersionInfo fw;
        fw.id = "bios";
        fw.version = biosVer;
        fw.purpose = FirmwarePurpose::Other;
        versions.push_back(fw);
        LOG_INFO("FirmwareInventory: bios = %s", biosVer.c_str());
    }

    LOG_INFO("FirmwareInventory: Total %zu firmware entries", versions.size());
    return versions;
}

std::vector<std::string> RedisAdapter::keys(redisContext* ctx,
                                              const std::string& pattern)
{
    std::vector<std::string> result;

    redisReply* reply = static_cast<redisReply*>(
        redisCommand(ctx, "KEYS %s", pattern.c_str()));

    if (!reply)
    {
        LOG_WARNING("Redis KEYS %s failed: %s", pattern.c_str(), ctx->errstr);
        return result;
    }

    if (reply->type == REDIS_REPLY_ERROR)
    {
        LOG_WARNING("Redis KEYS %s answered with an error: %s",
                    pattern.c_str(), reply->str);
    }

    if (reply->type == REDIS_REPLY_ARRAY)
    {
        for (size_t i = 0; i < reply->elements; i++)
        {
            if (reply->element[i]->type == REDIS_REPLY_STRING)
            {
                result.emplace_back(reply->element[i]->str,
                                    reply->element[i]->len);
            }
        }
    }

    freeReplyObject(reply);
    return result;
}

namespace
{

// Populate a LeakSensorInfo from a LIQUID_COOLING_INFO|<name> hash
// (written by thermalctld).
LeakSensorInfo parseLeakSensorFields(
    const std::string& sensorName,
    std::map<std::string, std::string>& fields)
{
    LeakSensorInfo sensor;
    sensor.name = sensorName;

    if (fields.count("leaking"))
    {
        sensor.leaking = fields["leaking"];
    }
    else if (fields.count("leak_status"))
    {
        // Legacy alias kept by thermalctld for backward compatibility
        sensor.leaking = fields["leak_status"];
    }
    if (fields.count("leak_sensor_status"))
    {
        sensor.leakSensorStatus = fields["leak_sensor_status"];
    }
    if (fields.count("leak_severity"))
    {
        sensor.leakSeverity = fields["leak_severity"];
    }
    if (fields.count("type"))
    {
        sensor.type = fields["type"];
    }
    if (fields.count("location"))
    {
        sensor.location = fields["location"];
    }

    return sensor;
}

} // namespace

std::vector<LeakSensorInfo> RedisAdapter::getLeakSensors()
{
    std::vector<LeakSensorInfo> sensors;

    if (!stateDbContext_)
    {
        return sensors;
    }

    // Discover all LIQUID_COOLING_INFO|* keys in STATE_DB
    auto keyList = keys(stateDbContext_, "LIQUID_COOLING_INFO|*");

    for (const auto& key : keyList)
    {
        // Extract sensor name from key (e.g., "LIQUID_COOLING_INFO|leakage1" -> "leakage1")
        size_t pos = key.find('|');
        if (pos == std::string::npos || pos + 1 >= key.size())
        {
            LOG_DEBUG("Skipping leak sensor key with no sensor name: %s",
                      key.c_str());
            continue;
        }
        std::string sensorName = key.substr(pos + 1);

        auto fields = hgetall(stateDbContext_, key);
        if (fields.empty())
        {
            LOG_WARNING("Leak sensor %s left out: no fields could be read",
                        key.c_str());
            continue;
        }

        auto sensor = parseLeakSensorFields(sensorName, fields);
        sensors.push_back(sensor);
        LOG_INFO("LeakSensor: %s leaking=%s severity=%s status=%s type=%s location=%s",
                 sensor.name.c_str(), sensor.leaking.c_str(),
                 sensor.leakSeverity.c_str(), sensor.leakSensorStatus.c_str(),
                 sensor.type.c_str(), sensor.location.c_str());
    }

    LOG_INFO("Found %zu leak sensors in STATE_DB", sensors.size());
    return sensors;
}

std::optional<LeakSensorInfo> RedisAdapter::getLeakSensor(
    const std::string& name)
{
    if (!stateDbContext_)
    {
        return std::nullopt;
    }

    std::string key = "LIQUID_COOLING_INFO|" + name;
    auto fields = hgetall(stateDbContext_, key);
    if (fields.empty())
    {
        return std::nullopt;
    }

    return parseLeakSensorFields(name, fields);
}

void RedisAdapter::freeReply(void* reply)
{
    if (reply)
    {
        freeReplyObject(reply);
    }
}

} // namespace sonic::dbus_bridge

