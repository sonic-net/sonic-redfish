#!/bin/bash
#######################################
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 Nexthop AI
# Copyright (C) 2026 SONiC Project
# Author: Nexthop AI
# Author: SONiC Project
# Author: Chinmoy Dey <chinmoy@nexthop.ai>
# License file: sonic-redfish/LICENSE
#######################################

# Launch sonic-dbus-bridge for the Redfish integration tests, wait until it
# owns its D-Bus names and print its pid.
#
# The one place that knows the bridge's config, platform and log file:
# start_services.sh and restart_bridge.py both start the bridge through it.

set -e

BRIDGE_CONFIG="/workspace/tests/redfish-api/data/config.yaml"
BRIDGE_LOG="/var/log/redfish-test/bridge.log"
BRIDGE_BUS_NAMES="xyz.openbmc_project.Inventory.Manager xyz.openbmc_project.State.Host"

mkdir -p "$(dirname "$BRIDGE_LOG")"

export PLATFORM="arm64-test-platform-r0"
sonic-dbus-bridge -c "$BRIDGE_CONFIG" >> "$BRIDGE_LOG" 2>&1 < /dev/null &
BRIDGE_PID=$!

bus_names_owned() {
    local names
    names=$(dbus-send --system --dest=org.freedesktop.DBus --print-reply \
        /org/freedesktop/DBus org.freedesktop.DBus.ListNames 2>/dev/null) || return 1
    for name in $BRIDGE_BUS_NAMES; do
        grep -q "\"$name\"" <<< "$names" || return 1
    done
}

for i in $(seq 1 120); do
    if bus_names_owned; then
        echo "$BRIDGE_PID"
        exit 0
    fi
    sleep 0.5
done

echo "FATAL: sonic-dbus-bridge (pid $BRIDGE_PID) did not claim its D-Bus names" >&2
echo "  bridge log tail:" >&2
tail -20 "$BRIDGE_LOG" >&2 2>/dev/null || true
exit 1
