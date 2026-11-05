#pragma once

#include "logger.hpp"
#include <hiredis/hiredis.h>
#include <swss/dbconnector.h>
#include <swss/redissocket.h>
#include <unistd.h>

namespace sonic::dbus_bridge
{

// An empty host explicitly selects this machine's configured Unix socket.
// A supplied host always selects TCP and never falls back to another server.
// The returned context belongs to the caller, including its descriptor.
inline redisContext* connectRedisEndpoint(const std::string& database,
                                         const std::string& host, int port)
{
    int descriptor = -1;
    try
    {
        redisContext* context = nullptr;
        if (host.empty())
        {
            const auto socket = swss::SonicDBConfig::getDbSock(database);
            descriptor = swss::openRedisUnixSocket(socket, 2000, true);
            context = redisConnectFd(descriptor);
            if (context)
            {
                descriptor = -1; // hiredis owns it from here.
            }
        }
        else
        {
            const timeval timeout{2, 0};
            context = redisConnectWithTimeout(host.c_str(), port, timeout);
        }

        if (descriptor >= 0)
        {
            close(descriptor);
            descriptor = -1;
        }
        if (context && context->err)
        {
            LOG_ERROR("Redis connection failed for %s: %s", database.c_str(),
                      context->errstr);
            redisFree(context);
            return nullptr;
        }
        const timeval timeout{2, 0};
        if (context && redisSetTimeout(context, timeout) != REDIS_OK)
        {
            LOG_ERROR("Cannot set Redis command timeout for %s", database.c_str());
            redisFree(context);
            return nullptr;
        }
        return context;
    }
    catch (const std::exception& error)
    {
        if (descriptor >= 0)
        {
            close(descriptor);
        }
        LOG_ERROR("Redis connection failed for %s: %s", database.c_str(),
                  error.what());
        return nullptr;
    }
}

} // namespace sonic::dbus_bridge
