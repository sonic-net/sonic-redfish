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
import pytest
import requests
from pathlib import Path
from data.redis_seed import DEVICE_METADATA
from framework.utils import resolve_dict, resolve_template, assert_subset, run_validators, extract_path

CASES_DIR = Path(__file__).parent.parent / "cases"

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
        with open(json_file) as f:
            data = json.load(f)
            for case in data:
                case_name = f"{json_file.stem}::{case['name']}"
                if "description" in case:
                    case_name += f" ({case['description']})"
                cases.append(pytest.param(case, id=case_name))
    return cases

@pytest.mark.parametrize("case", load_cases())
def test_redfish_api(case, redfish):
    """Generic JSON-driven test runner."""
    state = {}

    def _log_req(method, url, resp):
        test_logger.info(f"[REQUEST] {method.upper()} {url}")
        test_logger.info(f"[RESPONSE] Status: {resp.status_code}")
        try:
            test_logger.info(f"[RESPONSE] Body: {json.dumps(resp.json(), indent=2)}")
        except Exception:
            test_logger.info(f"[RESPONSE] Body: {resp.text}")

    test_logger.info(f"\n========== STARTING TEST: {case['name']} ==========")

    # Process prerequisites
    for prereq in case.get("prerequisite_calls", []):
        pre_endpoint = resolve_template(prereq["endpoint"], DEVICE_METADATA, state)
        pre_method = prereq.get("method", "GET").lower()
        req_func = getattr(redfish, pre_method)
        pre_resp = req_func(pre_endpoint)
        _log_req(pre_method, pre_endpoint, pre_resp)
        assert pre_resp.status_code == prereq.get("expected_status", 200)

        if "extract" in prereq:
            actual_resp = pre_resp.json()
            for state_key, json_path in prereq["extract"].items():
                state[state_key] = extract_path(actual_resp, json_path)

    endpoint = resolve_template(case["endpoint"], DEVICE_METADATA, state)
    method = case.get("method", "GET").lower()

    # Handle unauthenticated requests
    auth_enabled = case.get("auth", True)
    if not auth_enabled:
        full_url = f"https://localhost:443{endpoint}"
        resp = requests.request(
            method,
            full_url,
            verify=False,
            timeout=10
        )
        _log_req(method, endpoint, resp)
    else:
        req_func = getattr(redfish, method)
        resp = req_func(endpoint)
        _log_req(method, endpoint, resp)

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
