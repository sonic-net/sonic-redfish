#!/usr/bin/env python3
#######################################
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 Nexthop AI
# Copyright (C) 2026 SONiC Project
# Author: Nexthop AI
# Author: SONiC Project
# Author: Chinmoy Dey <chinmoy@nexthop.ai>
# License file: sonic-redfish/LICENSE
#######################################

"""Restart sonic-dbus-bridge inside the integration test container.

Usage:
    restart_bridge.py [--without-host-state]

Stops the running bridge, optionally deletes HOST_STATE|switch-host first so
the bridge comes up the way it does on a BMC where bmcctld has not published
the Switch-Host state yet, starts it again the way start_services.sh does and
waits until bmcweb answers through it. Used as an event_flow trigger step.
"""

import argparse
import os
import signal
import subprocess
import sys
import time

import redis
import requests
import urllib3

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

STATE_DB = 6
HOST_STATE_KEY = "HOST_STATE|switch-host"

BRIDGE_BINARY = "sonic-dbus-bridge"
BRIDGE_CONFIG = "/workspace/tests/redfish-api/data/config.yaml"
BRIDGE_LOG = "/var/log/redfish-test/bridge.log"
BRIDGE_PLATFORM = "arm64-test-platform-r0"
STATE_HOST_BUS_NAME = "xyz.openbmc_project.State.Host"

REDFISH_PROBE = "https://localhost:443/redfish/v1/Systems/system"
REDFISH_AUTH = ("bmcweb", "bmcweb")

STOP_TIMEOUT = 10
START_TIMEOUT = 60
POLL = 0.5


def _bridge_pids():
    out = subprocess.run(["pgrep", "-x", BRIDGE_BINARY],
                         capture_output=True, text=True).stdout.split()
    return [int(p) for p in out]


def _bus_name_owned(name):
    res = subprocess.run(
        ["dbus-send", "--system", "--dest=org.freedesktop.DBus", "--print-reply",
         "/org/freedesktop/DBus", "org.freedesktop.DBus.ListNames"],
        capture_output=True, text=True)
    return res.returncode == 0 and name in res.stdout


def _wait_until(predicate, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(POLL)
    return predicate()


def _redfish_answers():
    try:
        resp = requests.get(REDFISH_PROBE, auth=REDFISH_AUTH, verify=False, timeout=5)
    except requests.RequestException:
        return False
    return resp.status_code == 200


def stop_bridge():
    pids = _bridge_pids()
    for pid in pids:
        os.kill(pid, signal.SIGTERM)
    if not _wait_until(lambda: not _bridge_pids(), STOP_TIMEOUT):
        for pid in _bridge_pids():
            os.kill(pid, signal.SIGKILL)
        if not _wait_until(lambda: not _bridge_pids(), STOP_TIMEOUT):
            raise RuntimeError("sonic-dbus-bridge did not stop")
    if not _wait_until(lambda: not _bus_name_owned(STATE_HOST_BUS_NAME), STOP_TIMEOUT):
        raise RuntimeError(f"{STATE_HOST_BUS_NAME} still owned after the bridge stopped")
    return pids


def start_bridge():
    env = dict(os.environ, PLATFORM=BRIDGE_PLATFORM)
    log = open(BRIDGE_LOG, "ab")
    proc = subprocess.Popen([BRIDGE_BINARY, "-c", BRIDGE_CONFIG], env=env,
                            stdout=log, stderr=log, start_new_session=True)
    if not _wait_until(lambda: _bus_name_owned(STATE_HOST_BUS_NAME), START_TIMEOUT):
        raise RuntimeError(f"{STATE_HOST_BUS_NAME} not claimed within {START_TIMEOUT}s "
                           f"(bridge pid {proc.pid}, log {BRIDGE_LOG})")
    if not _wait_until(_redfish_answers, START_TIMEOUT):
        raise RuntimeError(f"bmcweb did not answer {REDFISH_PROBE} within {START_TIMEOUT}s "
                           f"of the bridge restart")
    return proc.pid


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    parser.add_argument("--without-host-state", action="store_true",
                        help=f"delete {HOST_STATE_KEY} before starting the bridge")
    args = parser.parse_args(argv[1:])

    if args.without_host_state:
        client = redis.StrictRedis(host="localhost", port=6379, db=STATE_DB,
                                   decode_responses=True)
        client.delete(HOST_STATE_KEY)
        print(f"{HOST_STATE_KEY} deleted")

    stopped = stop_bridge()
    print(f"stopped sonic-dbus-bridge pid(s) {stopped}")
    pid = start_bridge()
    print(f"started sonic-dbus-bridge pid {pid}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except RuntimeError as exc:
        print(f"restart_bridge: {exc}", file=sys.stderr)
        try:
            with open(BRIDGE_LOG) as f:
                print("".join(f.readlines()[-20:]), file=sys.stderr)
        except OSError:
            pass
        sys.exit(1)
