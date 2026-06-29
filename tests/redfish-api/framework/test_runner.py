#######################################
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 Nexthop AI
# Copyright (C) 2024 SONiC Project
# Author: Nexthop AI
# Author: SONiC Project
# License file: sonic-redfish/LICENSE
#######################################

import os
import json
import time
import pytest
import requests
from pathlib import Path
from data.redis_seed import DEVICE_METADATA
from framework.utils import resolve_dict, resolve_template, assert_subset, run_validators, extract_path
from framework.validator import validate_test_file

CASES_DIR = Path(__file__).parent.parent / "cases"

# Defaults for async-drain polling on redis_validations
DEFAULT_REDIS_TIMEOUT_SEC = 5.0
REDIS_POLL_INTERVAL_SEC = 0.05

import logging

# Setup a dedicated file logger for test_report.log
test_logger = logging.getLogger("redfish_test")
test_logger.setLevel(logging.DEBUG)
# ensure no propagation to root so it doesn't double-log
test_logger.propagate = False
# file handler
fh = logging.FileHandler("/workspace/test_report.log")
fh.setLevel(logging.DEBUG)
formatter = logging.Formatter('%(asctime)s - %(message)s')
fh.setFormatter(formatter)
test_logger.addHandler(fh)

def load_cases():
    """Load all JSON cases and yield pytest.param."""
    cases = []
    if not CASES_DIR.exists():
        return cases
    for json_file in CASES_DIR.glob("*.json"):
        errors = validate_test_file(json_file)
        if errors:
            error_msg = f"Validation failed for {json_file.name}:\n" + "\n".join(f"  - {e}" for e in errors)
            raise ValueError(error_msg)

        with open(json_file) as f:
            data = json.load(f)
            for case in data:
                case_name = f"{json_file.stem}::{case['name']}"
                if "description" in case:
                    case_name += f" ({case['description']})"
                cases.append(pytest.param(case, id=case_name))
    return cases

def _apply_redis_setup(setup_list, redis_dbs):
    """Run pre-conditions (currently: delete keys) before the request."""
    for entry in setup_list:
        db_name = entry["db"]
        action = entry["action"]
        db = redis_dbs[db_name]
        if action == "delete":
            keys = list(entry.get("keys") or [entry["key"]])
            deleted = db.delete(*keys)
            test_logger.info(
                f"[REDIS-SETUP] {db_name} DELETE {keys} -> {deleted} key(s) removed"
            )
        else:
            raise ValueError(f"Unknown redis_setup action: {action}")


def _wait_for_field(db, key, field, timeout):
    """Poll an HGET until the field is present or timeout elapses."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        v = db.hget(key, field)
        if v is not None:
            return v
        time.sleep(REDIS_POLL_INTERVAL_SEC)
    return None


def _field_error(actual, expected, db_name, key, field):
    """Compare a single hash field against an expectation.

    `expected` is either a scalar (str-compared) or a typed object:
        {"type": "float"|"int"|"bool"|"string", "equals": <value>}
        {"exists": true|false}

    Returns None on success, or a human-readable error string.
    """
    if isinstance(expected, dict):
        if "exists" in expected:
            want = bool(expected["exists"])
            present = actual is not None
            if want and not present:
                return f"{db_name}|{key}.{field}: expected to exist, missing"
            if not want and present:
                return f"{db_name}|{key}.{field}: expected absent, got '{actual}'"
            return None
        etype = expected.get("type", "string")
        eval_ = expected.get("equals")
        if actual is None:
            return f"{db_name}|{key}.{field}: missing (expected {expected})"
        try:
            if etype == "float" and float(actual) != float(eval_):
                return f"{db_name}|{key}.{field}: float mismatch, expected {eval_}, got '{actual}'"
            if etype == "int" and int(actual) != int(eval_):
                return f"{db_name}|{key}.{field}: int mismatch, expected {eval_}, got '{actual}'"
        except (TypeError, ValueError):
            return f"{db_name}|{key}.{field}: cannot parse '{actual}' as {etype}"
        if etype == "bool":
            want = "true" if eval_ else "false"
            if actual != want:
                return f"{db_name}|{key}.{field}: bool mismatch, expected {want}, got '{actual}'"
        elif etype == "string":
            if str(eval_) != actual:
                return f"{db_name}|{key}.{field}: mismatch, expected '{eval_}', got '{actual}'"
        return None
    # bare scalar -> string equality (decode_responses=True everywhere)
    if actual != str(expected):
        return f"{db_name}|{key}.{field}: mismatch, expected '{expected}', got '{actual}'"
    return None


def _run_redis_validations(validations, redis_dbs):
    """Validate Redis state after the request (key existence, async-drained fields)."""
    for v in validations:
        db_name = v["db"]
        db = redis_dbs[db_name]
        key = v["key"]
        timeout = float(v.get("timeout", DEFAULT_REDIS_TIMEOUT_SEC))

        if v.get("exists") is True:
            assert db.exists(key), \
                f"[REDIS-VALIDATE] expected key '{db_name}|{key}' to exist, missing"
            test_logger.info(f"[REDIS-VALIDATE] {db_name}|{key} exists: OK")
        if v.get("not_exists") is True:
            assert not db.exists(key), \
                f"[REDIS-VALIDATE] expected key '{db_name}|{key}' to NOT exist"
            test_logger.info(f"[REDIS-VALIDATE] {db_name}|{key} not_exists: OK")

        # Drain anchor: poll for one specific field before reading the rest.
        wait_field = v.get("wait_for_field")
        if wait_field is not None:
            val = _wait_for_field(db, key, wait_field, timeout)
            test_logger.info(
                f"[REDIS-VALIDATE] wait_for_field {db_name}|{key}.{wait_field} "
                f"(timeout={timeout}s) -> {val!r}"
            )
            assert val is not None, (
                f"[REDIS-VALIDATE] timed out after {timeout}s waiting for field "
                f"'{wait_field}' on '{db_name}|{key}'"
            )

        expected_fields = v.get("expected_fields") or {}
        if expected_fields:
            actual_fields = db.hgetall(key)
            test_logger.info(
                f"[REDIS-VALIDATE] HGETALL {db_name}|{key} -> "
                f"{json.dumps(actual_fields, indent=2, sort_keys=True)}"
            )
            errors = []
            for field, expected in expected_fields.items():
                err = _field_error(actual_fields.get(field), expected,
                                   db_name, key, field)
                if err:
                    errors.append(err)
            assert not errors, \
                "[REDIS-VALIDATE] field check(s) failed:\n  " + "\n  ".join(errors)


@pytest.mark.parametrize("case", load_cases())
def test_redfish_api(case, redfish, state_db, config_db):
    """Generic JSON-driven test runner."""
    state = {}
    redis_dbs = {"state_db": state_db, "config_db": config_db}

    def _log_req(method, url, body, resp):
        test_logger.info(f"[REQUEST] {method.upper()} {url}")
        if body is not None:
            test_logger.info(f"[REQUEST] Body: {json.dumps(body, indent=2)}")
        test_logger.info(f"[RESPONSE] Status: {resp.status_code}")
        try:
            test_logger.info(f"[RESPONSE] Body: {json.dumps(resp.json(), indent=2)}")
        except Exception:
            text = (resp.text or "").strip()
            test_logger.info(f"[RESPONSE] Body: {text!r}")

    test_logger.info(f"\n========== STARTING TEST: {case['name']} ==========")

    # Process prerequisites
    for prereq in case.get("prerequisite_calls", []):
        pre_endpoint = resolve_template(prereq["endpoint"], DEVICE_METADATA, state)
        pre_method = prereq.get("method", "GET").lower()
        req_func = getattr(redfish, pre_method)
        pre_resp = req_func(pre_endpoint)
        _log_req(pre_method, pre_endpoint, None, pre_resp)
        assert pre_resp.status_code == prereq.get("expected_status", 200)

        if "extract" in prereq:
            actual_resp = pre_resp.json()
            for state_key, json_path in prereq["extract"].items():
                state[state_key] = extract_path(actual_resp, json_path)

    # Redis pre-conditions (delete keys, etc.) applied AFTER the autouse
    # conftest reseed and BEFORE the request.
    if "redis_setup" in case:
        _apply_redis_setup(case["redis_setup"], redis_dbs)

    endpoint = resolve_template(case["endpoint"], DEVICE_METADATA, state)
    method = case.get("method", "GET").lower()
    body = case.get("body")
    if body is not None:
        body = resolve_dict(body, DEVICE_METADATA, state)

    # Handle unauthenticated requests
    auth_enabled = case.get("auth", True)
    if not auth_enabled:
        full_url = f"https://localhost:443{endpoint}"
        resp = requests.request(
            method,
            full_url,
            json=body,
            verify=False,
            timeout=10,
        )
        _log_req(method, endpoint, body, resp)
    else:
        req_func = getattr(redfish, method)
        kwargs = {"json": body} if body is not None else {}
        resp = req_func(endpoint, **kwargs)
        _log_req(method, endpoint, body, resp)

    assert resp.status_code == case.get("expected_status", 200), \
        f"Expected status {case.get('expected_status')}, got {resp.status_code}. Response: {resp.text}"

    if "expected_response" in case:
        expected_resp = resolve_dict(case["expected_response"], DEVICE_METADATA, state)
        actual_resp = resp.json()
        assert_subset(expected_resp, actual_resp)

    if "validators" in case:
        actual_resp = resp.json()
        run_validators(case["validators"], actual_resp, state)

    # Optional extraction for tests that still use it for self-validation
    if "extract" in case:
        actual_resp = resp.json()
        for state_key, json_path in case["extract"].items():
            state[state_key] = extract_path(actual_resp, json_path)

    # Post-request Redis assertions (run last so request/response logs land
    # in the report first, then the DB drain log).
    if "redis_validations" in case:
        _run_redis_validations(case["redis_validations"], redis_dbs)
