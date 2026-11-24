#######################################
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 Nexthop AI
# Copyright (C) 2024 SONiC Project
# Author: Nexthop AI
# Author: SONiC Project
# License file: sonic-redfish/LICENSE
#######################################

"""Audit trail tests.

bmcweb sends one audit record to syslog for every state-changing or refused
request. start_services.sh captures syslog to SYSLOG_FILE; these tests make
requests and read back the records each one added. Tests run one at a
time, so the lines added after a request belong to it.
"""

import base64
import json
import re
import subprocess
import time

import redis
import requests

from trigger_host_state import set_host_state
from trigger_leak import set_leak_state

BMCWEB_BASE = "https://localhost:443"
AUTH = ("bmcweb", "bmcweb")
TIMEOUT = 10

SYSLOG_FILE = "/var/log/redfish-test/syslog.log"
BMCWEB_LOG = "/var/log/redfish-test/bmcweb.log"
AUDIT_TAG = " redfish-audit["

RESET_URI = "/redfish/v1/Systems/system/Actions/ComputerSystem.Reset"
RESET_RULE = "/redfish/v1/Systems/<str>/Actions/ComputerSystem.Reset"
SESSIONS_URI = "/redfish/v1/SessionService/Sessions"
SUBSCRIPTIONS_URI = "/redfish/v1/EventService/Subscriptions"
SUBMIT_ALERT_URI = ("/redfish/v1/Managers/bmc/Oem/SONiC/RackManager"
                    "/Actions/SONiC.SubmitAlert")


def audit_lines():
    with open(SYSLOG_FILE, encoding="utf-8", errors="replace") as f:
        return [line.rstrip("\n") for line in f if AUDIT_TAG in line]


def audit_records():
    """Every audit record captured so far. A line that is not valid JSON
    fails the test: records must stay machine-readable."""
    return [json.loads(line.split("]: ", 1)[1]) for line in audit_lines()]


SETTLE = 0.3


def mark():
    """Number of audit lines so far; pass it to added_lines() later. Waits
    until the count stops moving, so a record still in delivery from the
    previous test is not counted as new."""
    count = len(audit_lines())
    while True:
        time.sleep(SETTLE)
        later = len(audit_lines())
        if later == count:
            return count
        count = later


# Records that no request causes: the once-a-minute telemetry summary, and
# the events the BMC raises itself (a power action is followed by host power
# events).
UNSOLICITED = ('"ev":"telemetry_summary"', '"ev":"event"')


def added_lines(since, wait=3.0, skip=UNSOLICITED):
    """Audit lines written after mark(), waiting briefly for delivery.
    Records not tied to a request are skipped."""
    def read():
        return [line for line in audit_lines()[since:]
                if not any(s in line for s in skip)]

    deadline = time.time() + wait
    while True:
        found = read()
        if found:
            # Read once more, so a second record that follows the first (a
            # forged line, say) is seen and not missed by returning early.
            time.sleep(SETTLE)
            return read()
        if time.time() >= deadline:
            return found
        time.sleep(0.1)


def added_events(since, message_id, wait=15.0):
    """Event records with this MessageId written after mark()."""
    deadline = time.time() + wait
    while True:
        found = [json.loads(line.split("]: ", 1)[1])
                 for line in audit_lines()[since:] if '"ev":"event"' in line]
        found = [rec for rec in found if rec["msg"] == message_id]
        if found or time.time() >= deadline:
            return found
        time.sleep(0.2)


def added_line(since):
    found = added_lines(since)
    assert len(found) == 1, f"expected one new audit record: {found}"
    return found[0]


def added_record(since):
    return json.loads(added_line(since).split("]: ", 1)[1])


def test_service_start_is_first_record():
    first = audit_records()[0]
    assert first["ev"] == "start"
    assert first["src"] == "bmcweb"
    assert first["seq"] == 1
    assert first["v"] == 1


def test_successful_get_is_not_recorded(redfish):
    before = mark()
    resp = redfish.get("/redfish/v1/Systems/system")
    assert resp.status_code == 200
    assert "X-Request-ID" not in resp.headers
    assert added_lines(before, wait=0.5) == []


LOG_LEVEL_PROPERTY = ["com.sonic.RackManager", "/com/sonic/Redfish/Logging",
                      "com.sonic.Redfish.Logging", "LogLevel"]
LOG_LEVEL_KEY = "LOGGER|sonic-dbus-bridge"


def config_db():
    return redis.StrictRedis(host="localhost", port=6379, db=4,
                             decode_responses=True)


def published_log_level():
    """The level the bridge publishes for bmcweb to follow."""
    out = subprocess.run(["busctl", "get-property", *LOG_LEVEL_PROPERTY],
                         capture_output=True, text=True, timeout=TIMEOUT)
    return out.stdout.strip()


def wait_for_log_level(level, wait=3.0):
    """The bridge is notified by Redis, so a change shows almost at once."""
    deadline = time.time() + wait
    while time.time() < deadline:
        if published_log_level() == f's "{level}"':
            return True
        time.sleep(0.2)
    return False


def test_log_level_defaults_to_info():
    assert published_log_level() == 's "INFO"'


def test_log_level_property_is_read_only():
    result = subprocess.run(
        ["busctl", "set-property", *LOG_LEVEL_PROPERTY, "s", "DEBUG"],
        capture_output=True, text=True, timeout=TIMEOUT)
    assert result.returncode != 0
    assert published_log_level() == 's "INFO"'


def test_unknown_log_level_in_config_db_is_ignored():
    config_db().hset(LOG_LEVEL_KEY, "LOGLEVEL", "TRACE")
    try:
        assert not wait_for_log_level("TRACE", wait=1.5)
        assert published_log_level() == 's "INFO"'
    finally:
        config_db().delete(LOG_LEVEL_KEY)


def test_get_is_recorded_only_at_debug_level(redfish):
    config_db().hset(LOG_LEVEL_KEY, "LOGLEVEL", "DEBUG")
    try:
        assert wait_for_log_level("DEBUG")
        time.sleep(0.5)
        before = mark()
        resp = redfish.get("/redfish/v1/Systems/system")
        assert resp.status_code == 200
        line = added_line(before)
        rec = json.loads(line.split("]: ", 1)[1])
        assert rec["ev"] == "read"
        assert rec["method"] == "GET"
        assert rec["uri"] == "/redfish/v1/Systems/<str>"
        assert rec["user"] == "bmcweb"
        assert rec["status"] == 200
        # authpriv.info, one step below the notice used for changes
        assert line.startswith("<86>")
    finally:
        # Removing the entry returns the bridge to its default level.
        config_db().delete(LOG_LEVEL_KEY)
    assert wait_for_log_level("INFO")
    time.sleep(0.5)
    before = mark()
    assert redfish.get("/redfish/v1/Systems/system").status_code == 200
    assert added_lines(before, wait=0.5) == []


def test_power_action_is_recorded(redfish):
    before = mark()
    resp = redfish.post(RESET_URI, json={"ResetType": "PowerCycle"})
    assert resp.status_code in (200, 204)
    rec = added_record(before)
    assert rec["ev"] == "request"
    assert rec["src"] == "bmcweb"
    assert rec["user"] == "bmcweb"
    assert rec["role"] == "Administrator"
    assert rec["auth"] == "basic"
    assert rec["method"] == "POST"
    assert rec["uri"] == RESET_RULE
    assert rec["action"] == "ComputerSystem.Reset"
    assert rec["detail"] == {"ResetType": "PowerCycle"}
    assert rec["status"] == resp.status_code
    assert re.fullmatch(r"c-[0-9a-f]{8}", rec["client"])
    assert rec["ms"] >= 0
    assert re.fullmatch(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z", rec["ts"])


def test_rejected_power_action_is_recorded_with_message_id(redfish):
    before = mark()
    resp = redfish.post(RESET_URI, json={"ResetType": "ForceRestart"})
    assert resp.status_code == 400
    rec = added_record(before)
    assert rec["ev"] == "request"
    assert rec["status"] == 400
    assert rec["detail"] == {"ResetType": "ForceRestart"}
    assert rec["msg"].endswith("ActionParameterNotSupported")


def test_hostile_reset_type_cannot_forge_a_record(redfish):
    hostile = 'On"}\n<85>Jan  1 00:00:00 redfish-audit[1]: {"ev":"request"'
    before = mark()
    resp = redfish.post(RESET_URI, json={"ResetType": hostile})
    assert added_record(before)["detail"] == {"ResetType": "<invalid>"}


def test_unknown_reset_type_is_not_copied(redfish):
    before = mark()
    resp = redfish.post(RESET_URI, json={"ResetType": "NotARealType"})
    assert added_record(before)["detail"] == {"ResetType": "<invalid>"}


def test_unauthenticated_request_is_denied_and_recorded():
    before = mark()
    resp = requests.get(f"{BMCWEB_BASE}/redfish/v1/Systems/system",
                        verify=False, timeout=TIMEOUT)
    assert resp.status_code == 401
    rec = added_record(before)
    assert rec["ev"] == "denied"
    assert rec["status"] == 401
    assert rec["method"] == "GET"
    assert rec["uri"] == "<unauthenticated>"
    assert "user" not in rec


def test_wrong_password_is_denied_without_user_name():
    before = mark()
    resp = requests.post(f"{BMCWEB_BASE}{RESET_URI}",
                         json={"ResetType": "On"},
                         auth=("bmcweb", "S3cret-Wrong-Pw"),
                         verify=False, timeout=TIMEOUT)
    assert resp.status_code == 401
    rec = added_record(before)
    assert rec["ev"] == "denied"
    assert "user" not in rec
    assert "detail" not in rec


def test_session_login_and_logout_are_recorded():
    before = mark()
    login = requests.post(f"{BMCWEB_BASE}{SESSIONS_URI}",
                          json={"UserName": AUTH[0], "Password": AUTH[1]},
                          verify=False, timeout=TIMEOUT)
    assert login.status_code == 201
    token = login.headers["X-Auth-Token"]
    rec = added_record(before)
    assert rec["ev"] == "login"
    assert rec["user"] == "bmcweb"
    assert rec["auth"] == "session"
    assert "detail" not in rec

    headers = {"X-Auth-Token": token}
    before = mark()
    action = requests.post(f"{BMCWEB_BASE}{RESET_URI}",
                           json={"ResetType": "On"}, headers=headers,
                           verify=False, timeout=TIMEOUT)
    action_rec = added_record(before)
    assert action_rec["auth"] == "session"
    assert re.fullmatch(r"s-[0-9a-f]{8}", action_rec["sess"])

    before = mark()
    logout = requests.delete(f"{BMCWEB_BASE}{login.headers['Location']}",
                             headers=headers, verify=False, timeout=TIMEOUT)
    assert logout.status_code in (200, 204)
    out_rec = added_record(before)
    assert out_rec["ev"] == "logout"
    assert out_rec["user"] == "bmcweb"
    assert out_rec["sess"] == action_rec["sess"]
    assert out_rec["uri"] == "/redfish/v1/SessionService/Sessions/<str>"

    assert all(token not in line for line in audit_lines())


def test_failed_session_login_is_recorded_without_user_name():
    before = mark()
    resp = requests.post(f"{BMCWEB_BASE}{SESSIONS_URI}",
                         json={"UserName": "typed-my-password-here",
                               "Password": "S3cret-Wrong-Pw"},
                         verify=False, timeout=TIMEOUT)
    assert resp.status_code == 401
    line = added_line(before)
    rec = json.loads(line.split("]: ", 1)[1])
    assert rec["ev"] == "login_failed"
    assert "user" not in rec
    assert "typed-my-password-here" not in line


def test_patch_records_property_names_not_values(redfish):
    before = mark()
    resp = redfish.patch("/redfish/v1/AccountService",
                         json={"AccountLockoutThreshold": 4242})
    rec = added_record(before)
    assert rec["method"] == "PATCH"
    assert rec["detail"] == {"changed": ["AccountLockoutThreshold"]}
    assert "4242" not in added_line(before)


def test_subscription_destination_is_not_recorded(redfish):
    before = mark()
    resp = redfish.post(SUBSCRIPTIONS_URI, json={
        "Destination": "http://192.0.2.77:8080/secret-path",
        "Protocol": "Redfish",
        "Context": "ctx-secret-1",
        "ResourceTypes": ["ComputerSystem"],
    })
    assert resp.status_code == 201
    location = resp.headers["Location"]
    try:
        rec = added_record(before)
        assert rec["detail"] == {
            "id": location.rstrip("/").rsplit("/", 1)[1],
            "Protocol": "Redfish",
            "ResourceTypes": ["ComputerSystem"],
        }
        line = added_line(before)
        for secret in ("192.0.2.77", "secret-path", "ctx-secret-1"):
            assert secret not in line
    finally:
        before = mark()
        deleted = redfish.delete(location)
    del_rec = added_record(before)
    assert del_rec["method"] == "DELETE"
    assert del_rec["detail"] == {"id": rec["detail"]["id"]}


def test_submit_alert_records_names_and_severity_only(redfish):
    before = mark()
    resp = redfish.post(SUBMIT_ALERT_URI, json={"redfish_alert_data": {
        "LiquidPressureDeviation": {
            "LiquidPressure": 68123, "Severity": "Major", "RscmPosition": 1},
    }})
    rec = added_record(before)
    assert rec["action"] == "SONiC.SubmitAlert"
    assert rec["detail"]["alerts"] == [
        {"name": "LiquidPressureDeviation", "Severity": "Major"}]
    assert rec["detail"]["bytes"] > 0
    assert "68123" not in added_line(before)


def state_db():
    return redis.StrictRedis(host="localhost", port=6379, db=6,
                             decode_responses=True)


def test_leak_is_recorded_without_any_subscriber():
    """A leak is not a request: it must still be on record, at the default
    log level and with nobody subscribed."""
    db = state_db()
    before = mark()
    set_leak_state(db, "leak_sensor_1", "Critical")
    try:
        events = added_events(
            before, "Environmental.1.1.0.LeakDetectedCritical")
        assert len(events) == 1, events
        assert events[0]["src"] == "bmcweb"
        assert events[0]["detail"]["resource"] == "leak_sensor_1"
        assert events[0]["detail"]["Severity"] == "Critical"
        assert events[0]["detail"]["subscribers"] >= 0
    finally:
        before = mark()
        set_leak_state(db, "leak_sensor_1", "OK")
        assert added_events(before, "Environmental.1.1.0.LeakDetectedNormal")


def test_host_power_change_is_recorded():
    db = state_db()
    before = mark()
    set_host_state(db, "Off")
    try:
        events = added_events(before, "ResourceEvent.1.3.0.ResourcePoweredOff")
        assert len(events) == 1, events
        assert events[0]["detail"]["resource"] == "system"
    finally:
        before = mark()
        set_host_state(db, "On")
        assert added_events(before, "ResourceEvent.1.3.0.ResourcePoweredOn")


def test_unrouted_path_is_not_copied_into_the_log(redfish):
    before = mark()
    resp = redfish.post("/redfish/v1/No/Such/Route-with-text-xyzzy", json={})
    rec = added_record(before)
    assert rec["status"] in (404, 405)
    # bmcweb's catch-all route answers unknown Redfish paths; the record
    # names that route, never the path the caller sent.
    assert rec["uri"] in ("/redfish/<path>", "<unmatched>")
    assert "xyzzy" not in added_line(before)


def test_sequence_numbers_have_no_gaps():
    seqs = [r["seq"] for r in audit_records() if r["src"] == "bmcweb"]
    assert seqs == list(range(1, len(seqs) + 1))
    assert all(r["ev"] != "gap" for r in audit_records())


def test_no_secret_reaches_any_log():
    """Runs after the tests above (definition order), which sent each of
    these values to the service at least once."""
    login = requests.post(f"{BMCWEB_BASE}{SESSIONS_URI}",
                          json={"UserName": AUTH[0], "Password": AUTH[1]},
                          verify=False, timeout=TIMEOUT)
    token = login.headers["X-Auth-Token"]
    requests.delete(f"{BMCWEB_BASE}{login.headers['Location']}",
                    headers={"X-Auth-Token": token}, verify=False,
                    timeout=TIMEOUT)
    time.sleep(0.5)

    basic = base64.b64encode(f"{AUTH[0]}:{AUTH[1]}".encode()).decode()
    secrets = [token, basic, "S3cret-Wrong-Pw", "127.0.0.1", "192.0.2.77",
               "secret-path", "ctx-secret-1", "typed-my-password-here"]
    audit_text = "\n".join(audit_lines())
    with open(BMCWEB_LOG, encoding="utf-8", errors="replace") as f:
        service_text = f.read()
    for secret in secrets:
        assert secret not in audit_text, f"{secret!r} found in audit log"
        assert secret not in service_text, f"{secret!r} found in bmcweb log"
