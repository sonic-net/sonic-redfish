#######################################
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 Nexthop AI
# Copyright (C) 2024 SONiC Project
# Author: Nexthop AI
# Author: SONiC Project
# License file: sonic-redfish/LICENSE
#######################################

"""
JSON Test Framework Validator for Redfish API Integration Tests.
This module provides syntactic and semantic validation for JSON-driven test case definitions
to prevent invalid test definitions from being checked in, catch mistakes early before runtime,
and enforce consistency across Redfish test definitions.
"""

import json
import re
from pathlib import Path

ALLOWED_METHODS = {"GET", "POST", "PATCH", "PUT", "DELETE"}
ALLOWED_KEYS = {
    "name", "description", "method", "endpoint", 
    "expected_status", "expected_response", "validators", 
    "prerequisite_calls", "auth"
}
REQUIRED_KEYS = {"name", "description", "method", "endpoint", "expected_status"}

EVENT_FLOW_ALLOWED_KEYS = {
    "name", "description", "type", "subscribe", "trigger", "expect_event", "teardown",
}
EVENT_FLOW_REQUIRED_KEYS = {
    "name", "description", "type", "subscribe", "trigger", "expect_event",
}


def _validate_event_flow_case(name: str, case: dict) -> list[str]:
    """Validate an event_flow case. Returns a list of error strings."""
    errors = []

    missing = EVENT_FLOW_REQUIRED_KEYS - set(case.keys())
    if missing:
        errors.append(f"Case '{name}': Missing required keys: {', '.join(sorted(missing))}")
    unknown = set(case.keys()) - EVENT_FLOW_ALLOWED_KEYS
    if unknown:
        errors.append(f"Case '{name}': Unknown keys found: {', '.join(sorted(unknown))}")

    sub = case.get("subscribe")
    if not isinstance(sub, dict):
        errors.append(f"Case '{name}': 'subscribe' must be an object.")
    else:
        if "endpoint" not in sub:
            errors.append(f"Case '{name}': 'subscribe' must contain 'endpoint'.")
        if sub.get("method") not in ALLOWED_METHODS:
            errors.append(f"Case '{name}': 'subscribe.method' invalid or missing.")

    trigger = case.get("trigger")
    if not isinstance(trigger, list):
        errors.append(f"Case '{name}': 'trigger' must be an array of step objects.")
    else:
        for s_idx, step in enumerate(trigger):
            if not isinstance(step, dict):
                errors.append(f"Case '{name}', trigger[{s_idx}]: must be an object.")
            elif "run" not in step and "wait_seconds" not in step:
                errors.append(f"Case '{name}', trigger[{s_idx}]: needs 'run' or 'wait_seconds'.")

    ee = case.get("expect_event")
    if not isinstance(ee, dict):
        errors.append(f"Case '{name}': 'expect_event' must be an object.")
    elif not isinstance(ee.get("validators"), list) or not ee.get("validators"):
        errors.append(f"Case '{name}': 'expect_event.validators' must be a non-empty array.")

    return errors


def validate_test_file(filepath: Path) -> list[str]:
    """
    Validates the structure and semantics of a Redfish JSON test file.
    This function processes the given JSON test definition and checks for both
    syntactic correctness (e.g., valid keys, types, structure) and semantic
    consistency (e.g., ensuring template variables like {{STATE.X}} are correctly
    extracted by prerequisite calls).
    """
    errors = []
    try:
        with open(filepath, 'r') as f:
            data = json.load(f)
    except json.JSONDecodeError as e:
        return [f"Invalid JSON: {e}"]

    if not isinstance(data, list):
        return ["Root element must be a JSON array."]

    seen_names = set()
    for idx, case in enumerate(data):
        if not isinstance(case, dict):
            errors.append(f"Case [{idx}]: Must be a JSON object.")
            continue
        
        name = case.get("name", f"unnamed_case_at_index_{idx}")
        if name in seen_names:
            errors.append(f"Case '{name}': Duplicate test name found.")
        seen_names.add(name)

        if case.get("type") == "event_flow":
            errors.extend(_validate_event_flow_case(name, case))
            continue

        #  Syntactic Key Validation
        missing_keys = REQUIRED_KEYS - set(case.keys())
        if missing_keys:
            errors.append(f"Case '{name}': Missing required keys: {', '.join(missing_keys)}")
        
        unknown_keys = set(case.keys()) - ALLOWED_KEYS
        if unknown_keys:
            errors.append(f"Case '{name}': Unknown keys found: {', '.join(unknown_keys)}")

        # Type and Value Validation
        if case.get("method") not in ALLOWED_METHODS:
            errors.append(f"Case '{name}': Invalid method '{case.get('method')}'. Allowed: {', '.join(ALLOWED_METHODS)}")
        
        if "expected_status" in case and not isinstance(case["expected_status"], int):
            errors.append(f"Case '{name}': 'expected_status' must be an integer.")

        # Extract logic and prerequisite validation
        extracted_states = set()
        if "prerequisite_calls" in case:
            if not isinstance(case["prerequisite_calls"], list):
                errors.append(f"Case '{name}': 'prerequisite_calls' must be an array.")
            else:
                for p_idx, prereq in enumerate(case["prerequisite_calls"]):
                    if not isinstance(prereq, dict):
                        errors.append(f"Case '{name}', Prereq [{p_idx}]: Must be an object.")
                        continue
                    if "endpoint" not in prereq:
                        errors.append(f"Case '{name}', Prereq [{p_idx}]: Missing 'endpoint'.")
                    if "extract" in prereq:
                        if not isinstance(prereq["extract"], dict):
                            errors.append(f"Case '{name}', Prereq [{p_idx}]: 'extract' must be an object.")
                        else:
                            extracted_states.update(prereq["extract"].keys())

        # Semantic Validation: State usage vs extraction
        case_str = json.dumps(case)
        used_states = set(re.findall(r"\{\{STATE\.([^}]+)\}\}", case_str))

        # Validators validation
        if "validators" in case:
            if not isinstance(case["validators"], list):
                errors.append(f"Case '{name}': 'validators' must be an array.")
            else:
                for v_idx, val in enumerate(case["validators"]):
                    if not isinstance(val, dict):
                        errors.append(f"Case '{name}', Validator [{v_idx}]: Must be an object.")
                        continue
                    if "type" not in val or "path" not in val:
                        errors.append(f"Case '{name}', Validator [{v_idx}]: Missing 'type' or 'path'.")
                    elif val["type"] in ["equals", "not_equals", "equals_state", "length_gte"] and "value" not in val:
                        errors.append(f"Case '{name}', Validator [{v_idx}]: Type '{val['type']}' requires a 'value' field.")
                    
                    if val.get("type") == "equals_state" and "value" in val:
                        used_states.add(val["value"])
        
        missing_states = used_states - extracted_states
        if missing_states:
            errors.append(f"Case '{name}': Uses {{STATE.*}} variables not extracted by any prerequisite: {', '.join(missing_states)}")

    return errors
