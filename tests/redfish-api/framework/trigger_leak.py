#!/usr/bin/env python3
#######################################
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 Nexthop AI
# Copyright (C) 2024 SONiC Project
# Author: Nexthop AI
# Author: SONiC Project
# License file: sonic-redfish/LICENSE
#######################################

"""Inject a leak-sensor state change into STATE_DB (db 6).

Usage:
    trigger_leak.py <sensor_name> <state>      # state: OK | Warning | Critical

The sonic-dbus-bridge watches LEAK_SENSOR|<name>, mirrors `state` onto the D-Bus
DetectorState property, and bmcweb turns the change into a Redfish event.
"""

import sys

import redis

STATE_DB = 6


def set_leak_state(client, sensor_name: str, state: str) -> None:
    """Write LEAK_SENSOR|<sensor_name> state into STATE_DB."""
    client.hset(f"LEAK_SENSOR|{sensor_name}", mapping={"state": state})


def main(argv) -> int:
    if len(argv) != 3:
        print(f"usage: {argv[0]} <sensor_name> <state>", file=sys.stderr)
        return 2
    sensor_name, state = argv[1], argv[2]
    client = redis.StrictRedis(host="localhost", port=6379, db=STATE_DB,
                               decode_responses=True)
    set_leak_state(client, sensor_name, state)
    print(f"LEAK_SENSOR|{sensor_name} state -> {state}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
