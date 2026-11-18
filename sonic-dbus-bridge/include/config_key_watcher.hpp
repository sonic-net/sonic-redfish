///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Author: Nexthop AI
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <hiredis/hiredis.h>

#include <functional>
#include <string>

namespace sonic::dbus_bridge
{

/**
 * @brief Watches one CONFIG_DB key through Redis keyspace notifications
 *
 * Runs on the caller's io_context: there is no thread and nothing is read
 * until Redis reports a change to the key. The callback gets no data; the
 * caller reads the key itself. It is also called after every (re)connect,
 * because a change made while disconnected raises no notification.
 */
class ConfigKeyWatcher
{
  public:
    /** Opens a new Redis connection; nullptr on failure. Owned by the watcher. */
    using Connect = std::function<redisContext*()>;

    ConfigKeyWatcher(boost::asio::io_context& io, std::string key,
                     Connect connect, std::function<void()> onChange);
    ~ConfigKeyWatcher();

    ConfigKeyWatcher(const ConfigKeyWatcher&) = delete;
    ConfigKeyWatcher& operator=(const ConfigKeyWatcher&) = delete;

    void start();
    void stop();

  private:
    void subscribe();
    bool keyspaceNotificationsEnabled();
    void waitForData();
    void readNotifications();
    void disconnect();
    void retryLater();

    std::string channel_;
    Connect connect_;
    std::function<void()> onChange_;
    redisContext* ctx_ = nullptr;
    boost::asio::posix::stream_descriptor socket_;
    boost::asio::steady_timer retryTimer_;
    bool stopped_ = true;
    bool failureLogged_ = false;
    bool notificationsEnabled_ = true;
};

} // namespace sonic::dbus_bridge
