///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
///////////////////////////////////////

#include <gtest/gtest.h>
#include "inventory_model.hpp"

namespace sonic::dbus_bridge::test {

// ---------------------------------------------------------------------------
// InventoryModelBuilder::build -- precedence: Redis > FRU > platform.json
// ---------------------------------------------------------------------------

TEST(InventoryModelBuilder, DefaultsWhenAllSourcesAbsent)
{
    auto model = InventoryModelBuilder::build(
        std::nullopt, std::nullopt, std::nullopt, std::nullopt);

    EXPECT_EQ(model.chassis.serialNumber, "Unknown");
    EXPECT_EQ(model.chassis.partNumber, "Unknown");
    EXPECT_EQ(model.chassis.manufacturer, "Unknown");
    EXPECT_EQ(model.chassis.model, "Unknown");
    EXPECT_EQ(model.chassis.serialNumberSource, FieldSource::Default);
    EXPECT_EQ(model.chassisState.powerState, "on");
    EXPECT_TRUE(model.psus.empty());
    EXPECT_TRUE(model.fans.empty());
}

TEST(InventoryModelBuilder, RedisBeatsFruForSerialNumber)
{
    FruInfo fru;
    fru.serialNumber = "FRU-SN";
    DeviceMetadata dm;
    dm.serialNumber = "REDIS-SN";

    auto model = InventoryModelBuilder::build(
        fru, dm, std::nullopt, std::nullopt);

    EXPECT_EQ(model.chassis.serialNumber, "REDIS-SN");
    EXPECT_EQ(model.chassis.serialNumberSource, FieldSource::Redis);
}

TEST(InventoryModelBuilder, FruUsedWhenRedisFieldsMissing)
{
    FruInfo fru;
    fru.serialNumber = "FRU-SN";
    fru.manufacturer = "FRU-Mfr";
    DeviceMetadata dm;  // no serial, no manufacturer

    auto model = InventoryModelBuilder::build(
        fru, dm, std::nullopt, std::nullopt);

    EXPECT_EQ(model.chassis.serialNumber, "FRU-SN");
    EXPECT_EQ(model.chassis.serialNumberSource, FieldSource::FruEeprom);
    EXPECT_EQ(model.chassis.manufacturer, "FRU-Mfr");
    EXPECT_EQ(model.chassis.manufacturerSource, FieldSource::FruEeprom);
}

TEST(InventoryModelBuilder, PartNumberFallsBackToHwsku)
{
    DeviceMetadata dm;
    dm.hwsku = "HWSKU-42";

    auto model = InventoryModelBuilder::build(
        std::nullopt, dm, std::nullopt, std::nullopt);

    EXPECT_EQ(model.chassis.partNumber, "HWSKU-42");
    EXPECT_EQ(model.chassis.partNumberSource, FieldSource::Redis);
}

TEST(InventoryModelBuilder, PartNumberPrefersRedisPartNumberOverHwsku)
{
    DeviceMetadata dm;
    dm.partNumber = "PN-Explicit";
    dm.hwsku = "HWSKU-42";

    auto model = InventoryModelBuilder::build(
        std::nullopt, dm, std::nullopt, std::nullopt);

    EXPECT_EQ(model.chassis.partNumber, "PN-Explicit");
}

TEST(InventoryModelBuilder, ModelFallsBackToPlatformWhenModelAbsent)
{
    DeviceMetadata dm;
    dm.platform = "arm64-accton";

    auto model = InventoryModelBuilder::build(
        std::nullopt, dm, std::nullopt, std::nullopt);

    EXPECT_EQ(model.chassis.model, "arm64-accton");
    EXPECT_EQ(model.chassis.modelSource, FieldSource::Redis);
}

TEST(InventoryModelBuilder, PlatformJsonPopulatesPsuAndFanLists)
{
    PlatformDescription pd;
    pd.chassisName = "Test Chassis";
    pd.psuNames = {"psu1", "psu2"};
    pd.fanNames = {"fan1", "fan2", "fan3"};

    auto model = InventoryModelBuilder::build(
        std::nullopt, std::nullopt, pd, std::nullopt);

    ASSERT_EQ(model.psus.size(), 2u);
    EXPECT_EQ(model.psus[0].name, "psu1");
    EXPECT_EQ(model.psus[1].name, "psu2");

    ASSERT_EQ(model.fans.size(), 3u);
    EXPECT_EQ(model.fans[2].name, "fan3");

    EXPECT_EQ(model.chassis.prettyName, "Test Chassis");
}

TEST(InventoryModelBuilder, ChassisStatePassedThrough)
{
    ChassisState cs;
    cs.powerState = "off";

    auto model = InventoryModelBuilder::build(
        std::nullopt, std::nullopt, std::nullopt, cs);

    EXPECT_EQ(model.chassisState.powerState, "off");
}

// ---------------------------------------------------------------------------
// SystemInfo precedence differs from ChassisInfo: FRU > CONFIG_DB (not Redis)
// ---------------------------------------------------------------------------

TEST(InventoryModelBuilder, SystemSerialPrefersFruOverRedis)
{
    FruInfo fru;
    fru.serialNumber = "FRU-SN";
    DeviceMetadata dm;
    dm.serialNumber = "REDIS-SN";

    auto model = InventoryModelBuilder::build(
        fru, dm, std::nullopt, std::nullopt);

    EXPECT_EQ(model.system.serialNumber, "FRU-SN");
}

TEST(InventoryModelBuilder, SystemHostnameFromConfigDb)
{
    DeviceMetadata dm;
    dm.hostname = "sonic-lab-01";

    auto model = InventoryModelBuilder::build(
        std::nullopt, dm, std::nullopt, std::nullopt);

    EXPECT_EQ(model.system.hostname, "sonic-lab-01");
    EXPECT_EQ(model.system.prettyName, "sonic-lab-01");
}

// ---------------------------------------------------------------------------
// hasChanged
// ---------------------------------------------------------------------------

TEST(HasChanged, ReturnsFalseForIdenticalModels)
{
    InventoryModel a;
    InventoryModel b;
    EXPECT_FALSE(hasChanged(a, b));
}

TEST(HasChanged, DetectsChassisSerialChange)
{
    InventoryModel a;
    InventoryModel b;
    b.chassis.serialNumber = "NEW-SN";
    EXPECT_TRUE(hasChanged(a, b));
}

TEST(HasChanged, DetectsPowerStateChange)
{
    InventoryModel a;
    InventoryModel b;
    b.chassisState.powerState = "off";
    EXPECT_TRUE(hasChanged(a, b));
}

TEST(HasChanged, IgnoresPsuFanListChanges)
{
    // hasChanged only compares chassis/system/chassisState fields --
    // this locks that behavior in so we notice if it changes.
    InventoryModel a;
    InventoryModel b;
    b.psus.push_back(PsuInfo{.name = "psu1"});
    EXPECT_FALSE(hasChanged(a, b));
}

} // namespace sonic::dbus_bridge::test

