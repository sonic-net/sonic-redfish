///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Author: Nexthop AI
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

#include "config_key_watcher.hpp"

#include "logger.hpp"

#include <chrono>
#include <utility>

namespace sonic::dbus_bridge
{

namespace
{
constexpr int configDbIndex = 4;
constexpr int retrySec = 30;
} // namespace

ConfigKeyWatcher::ConfigKeyWatcher(boost::asio::io_context& io,
                                   std::string key, Connect connect,
                                   std::function<void()> onChange)
    : channel_("__keyspace@" + std::to_string(configDbIndex) + "__:" +
               std::move(key)),
      connect_(std::move(connect)), onChange_(std::move(onChange)),
      socket_(io), retryTimer_(io)
{}

ConfigKeyWatcher::~ConfigKeyWatcher()
{
    stop();
}

void ConfigKeyWatcher::start()
{
    stopped_ = false;
    subscribe();
}

void ConfigKeyWatcher::stop()
{
    stopped_ = true;
    retryTimer_.cancel();
    disconnect();
}

void ConfigKeyWatcher::subscribe()
{
    ctx_ = connect_();
    redisReply* reply = nullptr;
    if (ctx_)
    {
        // Asked before SUBSCRIBE: a subscribed connection takes no other
        // command.
        notificationsEnabled_ = keyspaceNotificationsEnabled();
        reply = static_cast<redisReply*>(
            redisCommand(ctx_, "SUBSCRIBE %s", channel_.c_str()));
    }
    bool ok = reply && reply->type != REDIS_REPLY_ERROR;
    if (reply)
    {
        freeReplyObject(reply);
    }
    if (!ok)
    {
        if (!failureLogged_)
        {
            LOG_WARNING("Cannot watch %s; retrying every %d s",
                        channel_.c_str(), retrySec);
            failureLogged_ = true;
        }
        disconnect();
        retryLater();
        return;
    }
    failureLogged_ = false;
    LOG_INFO("Watching %s", channel_.c_str());
    if (!notificationsEnabled_)
    {
        LOG_WARNING("Redis keyspace notifications are off: changes to %s "
                    "apply only when the bridge restarts",
                    channel_.c_str());
    }
    socket_.assign(ctx_->fd);
    waitForData();
    // Pick up the current value, and any change made while not connected.
    onChange_();
}

// False only when Redis says key events are not published. If the setting
// cannot be read (CONFIG may be restricted) it is assumed to be on.
bool ConfigKeyWatcher::keyspaceNotificationsEnabled()
{
    redisReply* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "CONFIG GET notify-keyspace-events"));
    bool enabled = true;
    if (reply && reply->type == REDIS_REPLY_ARRAY && reply->elements == 2 &&
        reply->element[1]->type == REDIS_REPLY_STRING)
    {
        // 'K' turns on the __keyspace@<db>__ channels used here.
        std::string flags(reply->element[1]->str, reply->element[1]->len);
        enabled = flags.find('K') != std::string::npos;
    }
    if (reply)
    {
        freeReplyObject(reply);
    }
    return enabled;
}

void ConfigKeyWatcher::waitForData()
{
    socket_.async_wait(boost::asio::posix::stream_descriptor::wait_read,
                       [this](const boost::system::error_code& ec) {
                           if (!ec)
                           {
                               readNotifications();
                           }
                       });
}

void ConfigKeyWatcher::readNotifications()
{
    // The socket is readable, so this read returns without waiting.
    if (redisBufferRead(ctx_) != REDIS_OK)
    {
        LOG_WARNING("Lost the Redis connection watching %s", channel_.c_str());
        disconnect();
        retryLater();
        return;
    }

    bool changed = false;
    while (true)
    {
        void* reply = nullptr;
        if (redisGetReplyFromReader(ctx_, &reply) != REDIS_OK)
        {
            LOG_WARNING("Bad Redis reply watching %s", channel_.c_str());
            disconnect();
            retryLater();
            return;
        }
        if (!reply)
        {
            break; // nothing complete left in the buffer
        }
        // Any event on the key (hset, hdel, del, ...) means "read it again".
        changed = true;
        freeReplyObject(reply);
    }

    waitForData();
    if (changed)
    {
        onChange_();
    }
}

void ConfigKeyWatcher::disconnect()
{
    if (socket_.is_open())
    {
        // hiredis owns the descriptor: cancel the wait without closing it.
        boost::system::error_code ec;
        socket_.cancel(ec);
        socket_.release();
    }
    if (ctx_)
    {
        redisFree(ctx_);
        ctx_ = nullptr;
    }
}

void ConfigKeyWatcher::retryLater()
{
    if (stopped_)
    {
        return;
    }
    retryTimer_.expires_after(std::chrono::seconds(retrySec));
    retryTimer_.async_wait([this](const boost::system::error_code& ec) {
        if (!ec && !stopped_)
        {
            subscribe();
        }
    });
}

} // namespace sonic::dbus_bridge
