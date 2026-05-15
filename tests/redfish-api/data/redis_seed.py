#!/usr/bin/env python3
"""Seed Redis with deterministic test data for Redfish integration tests.

Populates CONFIG_DB (db 4) and STATE_DB (db 6) with known values so that
tests can assert against predictable Redfish responses.
"""

import redis
import sys


# ---------------------------------------------------------------------------
# Test constants -- tests/conftest.py imports these for assertions
# ---------------------------------------------------------------------------

DEVICE_METADATA = {
    "platform": "arm64-test-platform-r0",
    "hwsku": "TEST-HWSKU-001",
    "hostname": "sonic-test-switch",
    "mac": "00:11:22:33:44:55",
    "type": "StandAlone",
    "manufacturer": "TestManufacturer",
    "serial_number": "TST-SN-000001",
    "part_number": "TST-PN-000001",
    "model": "TestModel-1000",
}

FIRMWARE = {
    "SONIC_OS": {"version": "20240101.100"},
    "BIOS": {"version": "1.0.0-test"},
}

CHASSIS_STATE = {"power_state": "on"}


def seed(host: str = "localhost", port: int = 6379) -> None:
    config_db = redis.StrictRedis(host=host, port=port, db=4,
                                  decode_responses=True)
    state_db = redis.StrictRedis(host=host, port=port, db=6,
                                 decode_responses=True)

    # -- CONFIG_DB (4) -------------------------------------------------------
    config_db.flushdb()
    config_db.hset("DEVICE_METADATA|localhost", mapping=DEVICE_METADATA)

    # -- STATE_DB (6) --------------------------------------------------------
    state_db.flushdb()
    state_db.hset("CHASSIS_STATE|chassis0", mapping=CHASSIS_STATE)
    for name, fields in FIRMWARE.items():
        state_db.hset(f"BMC_FW_INVENTORY|{name}", mapping=fields)

    print("redis: test data seeded (CONFIG_DB=4, STATE_DB=6)")


if __name__ == "__main__":
    host = sys.argv[1] if len(sys.argv) > 1 else "localhost"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 6379
    seed(host, port)
