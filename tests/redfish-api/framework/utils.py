#######################################
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 Nexthop AI
# Copyright (C) 2024 SONiC Project
# Author: Nexthop AI
# Author: SONiC Project
# License file: sonic-redfish/LICENSE
#######################################

"""Utilities for Redfish JSON-driven testing."""
import json
import os
import re

SCHEMA_DIR = os.environ.get(
    "REDFISH_SCHEMA_DIR", "/usr/share/www/redfish/v1/JsonSchemas")

_schema_cache = {}


def _load_schema(schema_file):
    if schema_file not in _schema_cache:
        with open(os.path.join(SCHEMA_DIR, schema_file)) as f:
            _schema_cache[schema_file] = json.load(f)
    return _schema_cache[schema_file]


def _resolve_local_enum(doc, prop_schema):
    if not isinstance(prop_schema, dict):
        return None
    if "enum" in prop_schema:
        return prop_schema["enum"]
    ref = prop_schema.get("$ref", "")
    if ref.startswith("#/definitions/"):
        target = doc.get("definitions", {}).get(ref.split("/")[-1], {})
        return _resolve_local_enum(doc, target)
    for sub in prop_schema.get("anyOf", []):
        enum = _resolve_local_enum(doc, sub)
        if enum is not None:
            return enum
    return None


def assert_schema_conformance(obj, schema_file, definition):
    assert isinstance(obj, dict), \
        f"schema_conformance: response is not a JSON object (got {type(obj).__name__})"
    doc = _load_schema(schema_file)
    node = doc["definitions"][definition]
    props = node.get("properties", {})
    patterns = [re.compile(p) for p in node.get("patternProperties", {})]

    if node.get("additionalProperties") is False:
        for key in obj:
            allowed = key in props or any(p.search(key) for p in patterns)
            assert allowed, (
                f"schema_conformance: property '{key}' is not defined in "
                f"{schema_file}#/definitions/{definition} "
                f"(additionalProperties:false)")

    for req in node.get("required", []):
        assert req in obj, (
            f"schema_conformance: required property '{req}' missing "
            f"(per {schema_file}#/definitions/{definition})")

    for key, value in obj.items():
        if key in props and not isinstance(value, (dict, list)):
            enum = _resolve_local_enum(doc, props[key])
            if enum is not None:
                assert value in enum, (
                    f"schema_conformance: '{key}' value '{value}' is not in the "
                    f"schema enum {enum}")


def extract_path(data, path):
    """Simple JSONPath-like extractor. E.g., 'Members[0].@odata.id'"""
    if path == "$" or not path:
        return data
    keys = re.findall(r"\w+@odata\.\w+|@odata\.\w+|[\w]+|\[\d+\]", path)
    current = data
    for k in keys:
        if k.startswith("[") and k.endswith("]"):
            idx = int(k[1:-1])
            current = current[idx]
        else:
            current = current.get(k)
        if current is None:
            break
    return current

def resolve_template(value, seed_data, state):
    """Replace {{STATE.VAR}} or {{SEED.METADATA.key}} in string."""
    if not isinstance(value, str):
        return value

    def replacer(match):
        expr = match.group(1).strip()
        if expr.startswith("STATE."):
            key = expr.split(".", 1)[1]
            return str(state.get(key, match.group(0)))
        elif expr.startswith("SEED.DEVICE_METADATA."):
            key = expr.split(".", 2)[2]
            return str(seed_data.get(key, match.group(0)))
        return match.group(0)

    return re.sub(r"{{(.*?)}}", replacer, value)

def resolve_dict(data, seed_data, state):
    """Recursively resolve templates in a dictionary."""
    if isinstance(data, dict):
        return {k: resolve_dict(v, seed_data, state) for k, v in data.items()}
    elif isinstance(data, list):
        return [resolve_dict(v, seed_data, state) for v in data]
    else:
        return resolve_template(data, seed_data, state)

def assert_subset(expected, actual, path="root"):
    """Assert that expected is a subset of actual."""
    if isinstance(expected, dict):
        assert isinstance(actual, dict), f"{path}: Expected dict, got {type(actual)}"
        for k, v in expected.items():
            assert k in actual, f"{path}: Key '{k}' missing from actual response"
            assert_subset(v, actual[k], f"{path}.{k}")
    elif isinstance(expected, list):
        assert isinstance(actual, list), f"{path}: Expected list, got {type(actual)}"
        assert len(actual) >= len(expected), f"{path}: Expected list length >= {len(expected)}, got {len(actual)}"
        for i, v in enumerate(expected):
            assert_subset(v, actual[i], f"{path}[{i}]")
    else:
        assert expected == actual, f"{path}: Expected '{expected}', got '{actual}'"

def run_validators(validators, actual, state):
    """Run dynamic validators like exists, length_gte."""
    for val in validators:
        v_type = val.get("type")
        path = val.get("path")
        expected_val = val.get("value")

        extracted = extract_path(actual, path)

        if v_type == "exists":
            assert extracted is not None, f"Validator failed: Path '{path}' not found"
        elif v_type == "not_exists":
            assert extracted is None, f"Validator failed: Path '{path}' should not exist"
        elif v_type == "length_gte":
            assert len(extracted) >= expected_val, f"Validator failed: length of '{path}' < {expected_val}"
        elif v_type == "not_equals":
            assert extracted != expected_val, f"Validator failed: '{path}' equals {expected_val}"
        elif v_type == "equals_state":
            expected_state = state.get(expected_val)
            assert extracted == expected_state, f"Validator failed: '{path}' does not match state '{expected_val}'"
        elif v_type == "contains":
            assert isinstance(extracted, str), \
                f"Validator failed: '{path}' is not a string (got {type(extracted).__name__})"
            assert expected_val in extracted, \
                f"Validator failed: '{path}' ('{extracted}') does not contain '{expected_val}'"
        elif v_type == "not_contains":
            assert isinstance(extracted, str), \
                f"Validator failed: '{path}' is not a string (got {type(extracted).__name__})"
            assert expected_val not in extracted, \
                f"Validator failed: '{path}' ('{extracted}') unexpectedly contains '{expected_val}'"
        elif v_type == "schema_conformance":
            assert_schema_conformance(
                extracted, val["schema_file"], val["definition"])
        else:
            raise ValueError(f"Unknown validator type: {v_type}")
