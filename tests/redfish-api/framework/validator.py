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
    "prerequisite_calls", "auth",
    # POST/PATCH/PUT request body + Redis pre/post hooks
    "body", "redis_setup", "redis_validations",
}
REQUIRED_KEYS = {"name", "description", "method", "endpoint", "expected_status"}
ALLOWED_REDIS_DBS = {"state_db", "config_db"}
ALLOWED_REDIS_ACTIONS = {"delete"}
ALLOWED_FIELD_TYPES = {"string", "int", "float", "bool"}

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

        # HTTP 204 No Content forbids a response body (RFC 7230 §3.3.3).
        # Asserting a body against it is always a bug.
        if case.get("expected_status") == 204 and "expected_response" in case:
            errors.append(
                f"Case '{name}': 'expected_response' is not allowed when "
                f"'expected_status' is 204 (No Content has no body)."
            )

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
                    elif val["type"] in ["equals", "not_equals", "equals_state", "length_gte", "contains_member"] and "value" not in val:
                        errors.append(f"Case '{name}', Validator [{v_idx}]: Type '{val['type']}' requires a 'value' field.")
                    
                    if val.get("type") == "equals_state" and "value" in val:
                        used_states.add(val["value"])
        
        missing_states = used_states - extracted_states
        if missing_states:
            errors.append(f"Case '{name}': Uses {{STATE.*}} variables not extracted by any prerequisite: {', '.join(missing_states)}")

        # body (request payload for POST/PATCH/PUT)
        if "body" in case:
            method = case.get("method", "GET")
            if method not in {"POST", "PATCH", "PUT"}:
                errors.append(f"Case '{name}': 'body' is only valid for POST/PATCH/PUT (got '{method}').")
            if not isinstance(case["body"], (dict, list)):
                errors.append(f"Case '{name}': 'body' must be a JSON object or array.")

        # redis_setup: list of pre-condition ops applied before the request
        if "redis_setup" in case:
            if not isinstance(case["redis_setup"], list):
                errors.append(f"Case '{name}': 'redis_setup' must be an array.")
            else:
                for s_idx, entry in enumerate(case["redis_setup"]):
                    loc = f"Case '{name}', redis_setup[{s_idx}]"
                    if not isinstance(entry, dict):
                        errors.append(f"{loc}: Must be an object.")
                        continue
                    if entry.get("db") not in ALLOWED_REDIS_DBS:
                        errors.append(f"{loc}: 'db' must be one of {sorted(ALLOWED_REDIS_DBS)}.")
                    if entry.get("action") not in ALLOWED_REDIS_ACTIONS:
                        errors.append(f"{loc}: 'action' must be one of {sorted(ALLOWED_REDIS_ACTIONS)}.")
                    if entry.get("action") == "delete":
                        if "key" not in entry and "keys" not in entry:
                            errors.append(f"{loc}: delete requires 'key' or 'keys'.")
                        if "keys" in entry and not isinstance(entry["keys"], list):
                            errors.append(f"{loc}: 'keys' must be an array.")

        # redis_validations: list of post-request DB checks
        if "redis_validations" in case:
            if not isinstance(case["redis_validations"], list):
                errors.append(f"Case '{name}': 'redis_validations' must be an array.")
            else:
                for r_idx, rv in enumerate(case["redis_validations"]):
                    loc = f"Case '{name}', redis_validations[{r_idx}]"
                    if not isinstance(rv, dict):
                        errors.append(f"{loc}: Must be an object.")
                        continue
                    if rv.get("db") not in ALLOWED_REDIS_DBS:
                        errors.append(f"{loc}: 'db' must be one of {sorted(ALLOWED_REDIS_DBS)}.")
                    if "key" not in rv or not isinstance(rv["key"], str):
                        errors.append(f"{loc}: 'key' (string) is required.")
                    if "expected_fields" in rv and not isinstance(rv["expected_fields"], dict):
                        errors.append(f"{loc}: 'expected_fields' must be an object.")
                    if "timeout" in rv and not isinstance(rv["timeout"], (int, float)):
                        errors.append(f"{loc}: 'timeout' must be a number.")
                    if "wait_for_field" in rv and not isinstance(rv["wait_for_field"], str):
                        errors.append(f"{loc}: 'wait_for_field' must be a string.")
                    for fname, fexp in (rv.get("expected_fields") or {}).items():
                        if isinstance(fexp, dict) and "type" in fexp \
                                and fexp["type"] not in ALLOWED_FIELD_TYPES:
                            errors.append(
                                f"{loc}: field '{fname}' has unsupported type "
                                f"'{fexp['type']}' (allowed: {sorted(ALLOWED_FIELD_TYPES)})."
                            )

    return errors
