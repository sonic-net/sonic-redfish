///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2024 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

#pragma once

#include <sdbusplus/bus/match.hpp>

namespace redfish
{

class DbusLeakDetectorMonitor
{
  public:
    DbusLeakDetectorMonitor();
    sdbusplus::bus::match_t leakDetectorMonitor;
};

} // namespace redfish
