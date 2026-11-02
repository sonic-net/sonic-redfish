#!/usr/bin/env python3
#######################################
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 Nexthop AI
# Copyright (C) 2024 SONiC Project
# Author: Nexthop AI
# Author: SONiC Project
# Author: Chinmoy Dey <chinmoy@nexthop.ai>
# License file: sonic-redfish/LICENSE
#######################################

"""Inject a leak-sensor state change into STATE_DB (db 6).

Usage:
    trigger_leak.py <sensor_name> <state>      # state: OK | Warning | Critical | Unavailable

Writes the LIQUID_COOLING_INFO|<name> fields the way thermalctld does.
The sonic-dbus-bridge derives the D-Bus DetectorState from
leaking/leak_severity and Functional from leak_sensor_status, and bmcweb
turns the change into a Redfish event. Unavailable is a faulted sensor:
thermalctld writes leaking N/A and leak_sensor_status Fault.
"""

import sys

import redis

STATE_DB = 6

# Detector state -> thermalctld-style LIQUID_COOLING_INFO fields
STATE_TO_FIELDS = {
    "OK": {"leaking": "No", "leak_severity": "None", "leak_sensor_status": "Good"},
    "Warning": {"leaking": "Yes", "leak_severity": "MINOR", "leak_sensor_status": "Good"},
    "Critical": {"leaking": "Yes", "leak_severity": "CRITICAL", "leak_sensor_status": "Good"},
    "Unavailable": {"leaking": "N/A", "leak_severity": "None", "leak_sensor_status": "Fault"},
}


def set_leak_state(client, sensor_name: str, state: str) -> None:
    """Write LIQUID_COOLING_INFO|<sensor_name> leak fields into STATE_DB."""
    fields = dict(STATE_TO_FIELDS[state])
    fields["leak_status"] = fields["leaking"]  # legacy alias
    client.hset(f"LIQUID_COOLING_INFO|{sensor_name}", mapping=fields)


def main(argv) -> int:
    if len(argv) != 3:
        print(f"usage: {argv[0]} <sensor_name> <state>", file=sys.stderr)
        return 2
    sensor_name, state = argv[1], argv[2]
    if state not in STATE_TO_FIELDS:
        print(f"invalid state {state!r}; expected one of "
              f"{', '.join(STATE_TO_FIELDS)}", file=sys.stderr)
        return 2
    client = redis.StrictRedis(host="localhost", port=6379, db=STATE_DB,
                               decode_responses=True)
    set_leak_state(client, sensor_name, state)
    print(f"LIQUID_COOLING_INFO|{sensor_name} -> {state}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
