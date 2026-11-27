///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2024 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

#include "logger.hpp"

#include <gtest/gtest.h>

namespace logger = sonic::dbus_bridge::logger;
using logger::LogLevel;

TEST(LoggerLevel, DefaultIsInfo)
{
    EXPECT_EQ(logger::getLevel(), LogLevel::INFO);
    EXPECT_TRUE(logger::shouldLog(SYSLOG_INFO));
    EXPECT_FALSE(logger::shouldLog(SYSLOG_DEBUG));
}

TEST(LoggerLevel, NamesRoundTrip)
{
    // The names the CONFIG_DB LOGGER table uses.
    for (const char* name :
         {"DEBUG", "INFO", "NOTICE", "WARN", "ERROR", "CRIT"})
    {
        auto level = logger::parseLevel(name);
        ASSERT_TRUE(level.has_value()) << name;
        EXPECT_STREQ(logger::levelName(*level), name);
    }
}

TEST(LoggerLevel, SyslogSpellingsAndHigherLevelsAreAccepted)
{
    EXPECT_EQ(logger::parseLevel("WARNING"), LogLevel::WARNING);
    EXPECT_EQ(logger::parseLevel("ERR"), LogLevel::ERR);
    EXPECT_EQ(logger::parseLevel("ALERT"), LogLevel::CRIT);
    EXPECT_EQ(logger::parseLevel("EMERG"), LogLevel::CRIT);
}

TEST(LoggerLevel, NamesIgnoreCase)
{
    EXPECT_EQ(logger::parseLevel("debug"), LogLevel::DEBUG);
    EXPECT_EQ(logger::parseLevel("Info"), LogLevel::INFO);
}

TEST(LoggerLevel, SetLevelChangesWhatIsLogged)
{
    ASSERT_TRUE(logger::setLevel("DEBUG"));
    EXPECT_TRUE(logger::shouldLog(SYSLOG_DEBUG));
    ASSERT_TRUE(logger::setLevel("WARN"));
    EXPECT_FALSE(logger::shouldLog(SYSLOG_INFO));
    EXPECT_TRUE(logger::shouldLog(SYSLOG_ERR));
    ASSERT_TRUE(logger::setLevel("INFO"));
}

TEST(LoggerLevel, UnknownNameIsRejectedAndLevelKept)
{
    ASSERT_TRUE(logger::setLevel("NOTICE"));
    EXPECT_FALSE(logger::setLevel(""));
    EXPECT_FALSE(logger::setLevel("TRACE"));
    EXPECT_EQ(logger::getLevel(), LogLevel::NOTICE);
    ASSERT_TRUE(logger::setLevel("INFO"));
}
