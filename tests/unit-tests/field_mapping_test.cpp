///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2026 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

#include <gtest/gtest.h>
#include "field_mapping.hpp"

#include <set>
#include <string>
#include <utility>

namespace sonic::dbus_bridge::test {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// All Redis keys for telemetry must live under this prefix (single hash).
constexpr const char* TELEMETRY_REDIS_KEY_PREFIX = "RSCM_TELEMETRY|";

// Alert keys are split per alert kind: RSCM_ALERT|<AlertName>.
constexpr const char* ALERT_REDIS_KEY_PREFIX = "RSCM_ALERT|";

static bool isValidFieldType(FieldType t)
{
    switch (t)
    {
        case FieldType::String:
        case FieldType::Number:
        case FieldType::Integer:
        case FieldType::Boolean:
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Table stability
// ---------------------------------------------------------------------------

TEST(FieldMappingTables, ReturnSameStaticInstanceAcrossCalls)
{
    EXPECT_EQ(&getTelemetryMappings(), &getTelemetryMappings());
    EXPECT_EQ(&getAlertMappings(), &getAlertMappings());
}

TEST(FieldMappingTables, BothTablesAreNonEmpty)
{
    EXPECT_FALSE(getTelemetryMappings().empty());
    EXPECT_FALSE(getAlertMappings().empty());
}

// ---------------------------------------------------------------------------
// Entry-level invariants (apply to every row in every table)
// ---------------------------------------------------------------------------

static void checkPathSyntax(const std::string& path, const std::string& ctx,
                            const std::string& which)
{
    EXPECT_EQ(path.find(' '), std::string::npos)
        << ctx << ": " << which << " contains a space: '" << path << "'";
    EXPECT_NE(path.front(), '.') << ctx << ": " << which << " starts with '.'";
    EXPECT_NE(path.back(),  '.') << ctx << ": " << which << " ends with '.'";
}

static void checkEntryInvariants(const FieldMapping& m, const std::string& ctx)
{
    EXPECT_FALSE(m.jsonPath.empty())   << ctx << ": jsonPath is empty";
    EXPECT_FALSE(m.redisKey.empty())   << ctx << ": redisKey is empty";
    EXPECT_FALSE(m.redisField.empty()) << ctx << ": redisField is empty";

    // JSON paths use dot notation; spaces or leading/trailing dots are bugs.
    checkPathSyntax(m.jsonPath, ctx, "jsonPath");

    // altJsonPath is optional, but if present must follow the same syntax
    // rules and must not equal the primary path (would be a copy-paste bug).
    if (!m.altJsonPath.empty())
    {
        checkPathSyntax(m.altJsonPath, ctx, "altJsonPath");
        EXPECT_NE(m.altJsonPath, m.jsonPath)
            << ctx << ": altJsonPath equals jsonPath";
    }

    EXPECT_TRUE(isValidFieldType(m.type)) << ctx << ": invalid FieldType";
}

TEST(FieldMappingTables, TelemetryEntriesSatisfyInvariants)
{
    for (const auto& m : getTelemetryMappings())
    {
        checkEntryInvariants(m, "telemetry[" + m.jsonPath + "]");
    }
}

TEST(FieldMappingTables, AlertEntriesSatisfyInvariants)
{
    for (const auto& m : getAlertMappings())
    {
        checkEntryInvariants(m, "alert[" + m.jsonPath + "]");
    }
}

// ---------------------------------------------------------------------------
// Redis key namespacing -- telemetry vs alert tables are kept distinct
// to prevent cross-table collisions in STATE_DB.
// ---------------------------------------------------------------------------

TEST(FieldMappingTables, TelemetryKeysSharePrefix)
{
    for (const auto& m : getTelemetryMappings())
    {
        EXPECT_EQ(m.redisKey.rfind(TELEMETRY_REDIS_KEY_PREFIX, 0), 0u)
            << "telemetry key '" << m.redisKey
            << "' does not start with '" << TELEMETRY_REDIS_KEY_PREFIX << "'";
    }
}

TEST(FieldMappingTables, AlertKeysSharePrefix)
{
    for (const auto& m : getAlertMappings())
    {
        EXPECT_EQ(m.redisKey.rfind(ALERT_REDIS_KEY_PREFIX, 0), 0u)
            << "alert key '" << m.redisKey
            << "' does not start with '" << ALERT_REDIS_KEY_PREFIX << "'";
    }
}

// ---------------------------------------------------------------------------
// Duplicate detection: two rows must never target the same (key, field) --
// the second HSET would silently overwrite the first.
// ---------------------------------------------------------------------------

static void checkNoDuplicateKeyField(const std::vector<FieldMapping>& table,
                                     const std::string& tableName)
{
    std::set<std::pair<std::string, std::string>> seen;
    for (const auto& m : table)
    {
        auto inserted = seen.emplace(m.redisKey, m.redisField);
        EXPECT_TRUE(inserted.second)
            << tableName << ": duplicate (" << m.redisKey << ", "
            << m.redisField << ")";
    }
}

TEST(FieldMappingTables, NoDuplicateRedisTargetsInTelemetry)
{
    checkNoDuplicateKeyField(getTelemetryMappings(), "telemetry");
}

TEST(FieldMappingTables, NoDuplicateRedisTargetsInAlerts)
{
    checkNoDuplicateKeyField(getAlertMappings(), "alerts");
}

} // namespace sonic::dbus_bridge::test
