///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2024 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

#pragma once

#include <syslog.h>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <signal.h>
#include <unistd.h>

namespace sonic::dbus_bridge::logger
{

    // Log levels matching syslog priorities
    enum class LogLevel : int
    {
        DEBUG = LOG_DEBUG,     // 7
        INFO = LOG_INFO,       // 6
        NOTICE = LOG_NOTICE,   // 5
        WARNING = LOG_WARNING, // 4
        ERR = LOG_ERR,         // 3
        CRIT = LOG_CRIT        // 2
    };

    // Global logger state
    struct LoggerState
    {
        std::atomic<LogLevel> currentLevel{LogLevel::INFO};
        std::atomic<bool> fileLoggingEnabled{false};
        std::mutex fileMutex;
        FILE* logFile = nullptr;
        static constexpr const char* logFilePath = "/var/log/sonic-dbus-bridge.log";

        void enableFileLogging()
        {
            std::lock_guard<std::mutex> lock(fileMutex);
            if (!logFile)
            {
                logFile = fopen(logFilePath, "a");
                if (logFile)
                {
                    fileLoggingEnabled.store(true, std::memory_order_release);
                    syslog(6, "Logger: File logging enabled to %s", logFilePath); // LOG_INFO = 6
                }
                else
                {
                    syslog(3, "Logger: Failed to open log file %s: %s",  // LOG_ERR = 3
                            logFilePath, strerror(errno));
                }
            }
        }

        void disableFileLogging()
        {
            std::lock_guard<std::mutex> lock(fileMutex);
            if (logFile)
            {
                fclose(logFile);
                logFile = nullptr;
                fileLoggingEnabled.store(false, std::memory_order_release);
                // Delete the log file
                unlink(logFilePath);
                syslog(6, "Logger: File logging disabled and log file deleted"); // LOG_INFO = 6
            }
        }

        void writeToFile(const char* message)
        {
            std::lock_guard<std::mutex> lock(fileMutex);
            if (logFile && fileLoggingEnabled.load(std::memory_order_acquire))
            {
                // Get current timestamp
                time_t now = time(nullptr);
                char timestamp[64];
                strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S",
                        localtime(&now));

                fprintf(logFile, "[%s] %s\n", timestamp, message);
                fflush(logFile);
            }
        }

        ~LoggerState()
        {
            if (logFile)
            {
                fclose(logFile);
            }
        }
    };

    // Get the global logger state
    inline LoggerState& getLoggerState()
    {
        static LoggerState state;
        return state;
    }

    // Check if a log level should be logged
    inline bool shouldLog(int level)
    {
        return level <= static_cast<int>(getLoggerState().currentLevel.load(
                    std::memory_order_acquire));
    }

    // Level names of the CONFIG_DB LOGGER table (the names "config syslog
    // level" accepts), plus the syslog spellings WARNING and ERR.
    // Case is ignored, so a hand-typed "debug" is not silently dropped.
    inline std::optional<LogLevel> parseLevel(std::string_view text)
    {
        std::string name(text);
        for (char& c : name)
        {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        if (name == "DEBUG") { return LogLevel::DEBUG; }
        if (name == "INFO") { return LogLevel::INFO; }
        if (name == "NOTICE") { return LogLevel::NOTICE; }
        if (name == "WARN" || name == "WARNING") { return LogLevel::WARNING; }
        if (name == "ERROR" || name == "ERR") { return LogLevel::ERR; }
        // Nothing is logged above CRIT, so the two higher names map to it.
        if (name == "CRIT" || name == "ALERT" || name == "EMERG")
        {
            return LogLevel::CRIT;
        }
        return std::nullopt;
    }

    inline const char* levelName(LogLevel level)
    {
        switch (level)
        {
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO: return "INFO";
            case LogLevel::NOTICE: return "NOTICE";
            case LogLevel::WARNING: return "WARN";
            case LogLevel::ERR: return "ERROR";
            case LogLevel::CRIT: return "CRIT";
        }
        return "INFO";
    }

    inline LogLevel getLevel()
    {
        return getLoggerState().currentLevel.load(std::memory_order_acquire);
    }

    // Change the level at runtime. False (and no change) for an unknown name.
    inline bool setLevel(std::string_view name)
    {
        std::optional<LogLevel> level = parseLevel(name);
        if (!level)
        {
            return false;
        }
        getLoggerState().currentLevel.store(*level, std::memory_order_release);
        return true;
    }

    // Initialize logger from environment variable
    inline void initFromEnv()
    {
        const char* levelStr = std::getenv("SONIC_DBUS_BRIDGE_LOG_LEVEL");
        if (levelStr)
        {
            setLevel(levelStr);
        }
    }

} // namespace sonic::dbus_bridge::logger

// Undefine syslog constants so we can use them as macro names
// We'll use the numeric values directly in our macros
#undef LOG_DEBUG
#undef LOG_INFO
#undef LOG_NOTICE
#undef LOG_WARNING
#undef LOG_ERR
#undef LOG_CRIT

// Define numeric constants for syslog levels (from syslog.h)
#define SYSLOG_DEBUG 7
#define SYSLOG_INFO 6
#define SYSLOG_NOTICE 5
#define SYSLOG_WARNING 4
#define SYSLOG_ERR 3
#define SYSLOG_CRIT 2

// Public API macros for logging
#define LOG_DEBUG(fmt, ...)                                                      \
    do                                                                           \
{                                                                            \
    if (::sonic::dbus_bridge::logger::shouldLog(SYSLOG_DEBUG))               \
    {                                                                        \
        syslog(SYSLOG_DEBUG, fmt, ##__VA_ARGS__);                            \
        if (::sonic::dbus_bridge::logger::getLoggerState()                   \
                .fileLoggingEnabled.load(std::memory_order_acquire))         \
        {                                                                    \
            char _buf[1024];                                                 \
            snprintf(_buf, sizeof(_buf), fmt, ##__VA_ARGS__);                \
            ::sonic::dbus_bridge::logger::getLoggerState().writeToFile(_buf);\
        }                                                                    \
    }                                                                        \
} while (0)

#define LOG_INFO(fmt, ...)                                                       \
    do                                                                           \
{                                                                            \
    if (::sonic::dbus_bridge::logger::shouldLog(SYSLOG_INFO))                \
    {                                                                        \
        syslog(SYSLOG_INFO, fmt, ##__VA_ARGS__);                             \
        if (::sonic::dbus_bridge::logger::getLoggerState()                   \
                .fileLoggingEnabled.load(std::memory_order_acquire))         \
        {                                                                    \
            char _buf[1024];                                                 \
            snprintf(_buf, sizeof(_buf), fmt, ##__VA_ARGS__);                \
            ::sonic::dbus_bridge::logger::getLoggerState().writeToFile(_buf);\
        }                                                                    \
    }                                                                        \
} while (0)

#define LOG_NOTICE(fmt, ...)                                                     \
    do                                                                           \
{                                                                            \
    if (::sonic::dbus_bridge::logger::shouldLog(SYSLOG_NOTICE))              \
    {                                                                        \
        syslog(SYSLOG_NOTICE, fmt, ##__VA_ARGS__);                           \
        if (::sonic::dbus_bridge::logger::getLoggerState()                   \
                .fileLoggingEnabled.load(std::memory_order_acquire))         \
        {                                                                    \
            char _buf[1024];                                                 \
            snprintf(_buf, sizeof(_buf), fmt, ##__VA_ARGS__);                \
            ::sonic::dbus_bridge::logger::getLoggerState().writeToFile(_buf);\
        }                                                                    \
    }                                                                        \
} while (0)

#define LOG_WARNING(fmt, ...)                                                    \
    do                                                                           \
{                                                                            \
    if (::sonic::dbus_bridge::logger::shouldLog(SYSLOG_WARNING))             \
    {                                                                        \
        syslog(SYSLOG_WARNING, fmt, ##__VA_ARGS__);                          \
        if (::sonic::dbus_bridge::logger::getLoggerState()                   \
                .fileLoggingEnabled.load(std::memory_order_acquire))         \
        {                                                                    \
            char _buf[1024];                                                 \
            snprintf(_buf, sizeof(_buf), fmt, ##__VA_ARGS__);                \
            ::sonic::dbus_bridge::logger::getLoggerState().writeToFile(_buf);\
        }                                                                    \
    }                                                                        \
} while (0)

#define LOG_WARN(fmt, ...) LOG_WARNING(fmt, ##__VA_ARGS__)

#define LOG_ERROR(fmt, ...)                                                      \
    do                                                                           \
{                                                                            \
    if (::sonic::dbus_bridge::logger::shouldLog(SYSLOG_ERR))                 \
    {                                                                        \
        syslog(SYSLOG_ERR, fmt, ##__VA_ARGS__);                              \
        if (::sonic::dbus_bridge::logger::getLoggerState()                   \
                .fileLoggingEnabled.load(std::memory_order_acquire))         \
        {                                                                    \
            char _buf[1024];                                                 \
            snprintf(_buf, sizeof(_buf), fmt, ##__VA_ARGS__);                \
            ::sonic::dbus_bridge::logger::getLoggerState().writeToFile(_buf);\
        }                                                                    \
    }                                                                        \
} while (0)

#define LOG_ERR(fmt, ...) LOG_ERROR(fmt, ##__VA_ARGS__)

#define LOG_CRITICAL(fmt, ...)                                                   \
    do                                                                           \
{                                                                            \
    if (::sonic::dbus_bridge::logger::shouldLog(SYSLOG_CRIT))                \
    {                                                                        \
        syslog(SYSLOG_CRIT, fmt, ##__VA_ARGS__);                             \
        if (::sonic::dbus_bridge::logger::getLoggerState()                   \
                .fileLoggingEnabled.load(std::memory_order_acquire))         \
        {                                                                    \
            char _buf[1024];                                                 \
            snprintf(_buf, sizeof(_buf), fmt, ##__VA_ARGS__);                \
            ::sonic::dbus_bridge::logger::getLoggerState().writeToFile(_buf);\
        }                                                                    \
    }                                                                        \
} while (0)

#define LOG_CRIT(fmt, ...) LOG_CRITICAL(fmt, ##__VA_ARGS__)

// Initialization macro (call from bridge_app.cpp)
#define LOGGER_INIT() ::sonic::dbus_bridge::logger::initFromEnv()

// Signal handler helpers (to be called from bridge_app.cpp signal handlers)
#define LOGGER_ENABLE_FILE_LOGGING() \
    ::sonic::dbus_bridge::logger::getLoggerState().enableFileLogging()

#define LOGGER_DISABLE_FILE_LOGGING() \
    ::sonic::dbus_bridge::logger::getLoggerState().disableFileLogging()


