#!/bin/bash
#######################################
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 Nexthop AI
# Copyright (C) 2024 SONiC Project
# Author: Nexthop AI
# Author: SONiC Project
# License file: sonic-redfish/LICENSE
#######################################

# Start all services needed for Redfish integration tests.
#
# - dbus-daemon -> redis-server -> sonic-dbus-bridge -> bmcweb
#
# Each service waits for its dependency to be ready before starting.

set -e

BRIDGE_CONFIG="/workspace/tests/redfish-api/data/config.yaml"
PLATFORM_JSON="/workspace/tests/redfish-api/data/platform.json"
LOG_DIR="/var/log/redfish-test"

mkdir -p "$LOG_DIR" /run/dbus /var/lib/sonic-dbus-bridge

echo "=== Redfish integration test: starting services ==="

# -----------------
# D-Bus system bus
# -----------------
echo " Starting dbus-daemon..."
# Install D-Bus policy files so sonic-dbus-bridge can own its bus names
cp /workspace/sonic-dbus-bridge/dbus/*.conf /etc/dbus-1/system.d/
dbus-daemon --system --nofork --nopidfile &> "$LOG_DIR/dbus.log" &
DBUS_PID=$!

# Wait for the system bus socket to appear
for i in $(seq 1 30); do
    [ -S /run/dbus/system_bus_socket ] && break
    sleep 0.2
done
if [ ! -S /run/dbus/system_bus_socket ]; then
    echo "FATAL: dbus-daemon did not start" >&2
    exit 1
fi
echo "  dbus-daemon ready (pid=$DBUS_PID)"

# -----
# Redis
# -----
echo "Starting redis-server..."
# notify-keyspace-events KEA enables keyspace notifications (__keyspace@<db>__:<key>).
# sonic-dbus-bridge subscribes to these to react to runtime STATE_DB changes (e.g.
# LEAK_SENSOR state). Without it the bridge never sees writes made after startup,
# so event-driven flows (leak detection events) never fire.
redis-server --daemonize yes --logfile "$LOG_DIR/redis.log" --loglevel warning \
    --notify-keyspace-events KEA

for i in $(seq 1 30); do
    redis-cli ping 2>/dev/null | grep -q PONG && break
    sleep 0.2
done
if ! redis-cli ping 2>/dev/null | grep -q PONG; then
    echo "FATAL: redis-server did not start" >&2
    exit 1
fi
echo "  redis-server ready"

# -------------------------
# Seed Redis with test data
# -------------------------
echo "Seeding Redis test data..."
python3 /workspace/tests/redfish-api/data/redis_seed.py
echo "  test data seeded"

# ------------------
# sonic-dbus-bridge
# -----------------
echo "Starting sonic-dbus-bridge..."
export PLATFORM="arm64-test-platform-r0"
sonic-dbus-bridge -c "$BRIDGE_CONFIG" &> "$LOG_DIR/bridge.log" &
BRIDGE_PID=$!

# Wait for the bridge to register its D-Bus service
for i in $(seq 1 60); do
    if dbus-send --system --dest=org.freedesktop.DBus --print-reply \
        /org/freedesktop/DBus org.freedesktop.DBus.ListNames 2>/dev/null \
        | grep -q "xyz.openbmc_project.Inventory"; then
        break
    fi
    sleep 0.5
done
if ! dbus-send --system --dest=org.freedesktop.DBus --print-reply \
    /org/freedesktop/DBus org.freedesktop.DBus.ListNames 2>/dev/null \
    | grep -q "xyz.openbmc_project.Inventory"; then
    echo "FATAL: sonic-dbus-bridge may not be fully ready" >&2
    echo "  bridge log tail:" >&2
    tail -20 "$LOG_DIR/bridge.log" 2>/dev/null || true
    exit 1
fi
echo "  sonic-dbus-bridge started (pid=$BRIDGE_PID)"

# -------
# bmcweb
# -------
echo "Starting bmcweb..."
# bmcweb expects systemd socket activation (FileDescriptorName=bmcweb_443_https_auth
# in /usr/lib/systemd/system/bmcweb.socket). Without the LISTEN_FDS handoff it
# starts but never binds 443. systemd-socket-activate provides the FD outside
# of systemd so the daemon listens.
systemd-socket-activate -l 443 --fdname=bmcweb_443_https_auth \
    /usr/bin/bmcweb daemon &> "$LOG_DIR/bmcweb.log" &
BMCWEB_PID=$!

# Wait for bmcweb to accept connections on port 443
for i in $(seq 1 60); do
    if curl -sk --max-time 2 -o /dev/null \
        https://localhost:443/redfish/v1/ 2>/dev/null; then
        break
    fi
    sleep 0.5
done
if ! curl -sk --max-time 2 -o /dev/null \
    https://localhost:443/redfish/v1/ 2>/dev/null; then
    echo "FATAL: bmcweb may not be ready on port 443" >&2
    echo "  bmcweb log tail:" >&2
    tail -20 "$LOG_DIR/bmcweb.log" 2>/dev/null || true
    exit 1
fi
echo "  bmcweb started (pid=$BMCWEB_PID)"

echo ""
echo "=== All services started ==="
echo "  dbus-daemon : pid=$DBUS_PID"
echo "  redis       : daemonized"
echo "  bridge      : pid=$BRIDGE_PID"
echo "  bmcweb      : pid=$BMCWEB_PID"
echo "  logs        : $LOG_DIR/"
echo ""
