#######################################
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 Nexthop AI
# Copyright (C) 2024 SONiC Project
# Author: Nexthop AI
# Author: SONiC Project
# License file: sonic-redfish/LICENSE
#######################################

"""RMC-style event-flow engine.

Drives the leak-detection event path the way a Rack Management Controller would:
  1. start an in-process webhook listener,
  2. POST an EventService subscription whose Destination is the listener,
  3. run ordered trigger steps (commands / background scripts / waits),
  4. wait for the first received event payload that satisfies all validators,
  5. tear down (DELETE subscription, run teardown step, kill background procs).
"""

import copy
import shlex
import subprocess
import sys
import time

from framework.event_listener import EventListener
from framework.utils import run_validators

LISTENER_URL_TEMPLATE = "{{RMC.LISTENER_URL}}"


def build_cmd(run_str: str) -> list:
    """Split a `run` string into argv; prefix the Python interpreter for .py."""
    tokens = shlex.split(run_str)
    if tokens and tokens[0].endswith(".py"):
        return [sys.executable, *tokens]
    return tokens


def inject_listener_url(obj, url: str):
    """Deep-copy obj and replace the listener-URL template anywhere it appears."""
    def _walk(node):
        if isinstance(node, dict):
            return {k: _walk(v) for k, v in node.items()}
        if isinstance(node, list):
            return [_walk(v) for v in node]
        if isinstance(node, str):
            return node.replace(LISTENER_URL_TEMPLATE, url)
        return node
    return _walk(copy.deepcopy(obj))


def event_matches(payload, validators) -> bool:
    """True if `payload` satisfies every validator (run_validators raises on fail)."""
    try:
        run_validators(validators, payload, {})
        return True
    except AssertionError:
        return False


def run_trigger_steps(steps, repo_root, background_procs):
    """Execute trigger/teardown steps in order.

    Each step is one of:
      {"wait_seconds": N}            -> sleep
      {"run": "...", "background": False/absent} -> foreground, checked
      {"run": "...", "background": True}         -> Popen, appended to background_procs
    """
    for step in steps:
        if "wait_seconds" in step:
            time.sleep(step["wait_seconds"])
            continue
        cmd = build_cmd(step["run"])
        if step.get("background"):
            background_procs.append(subprocess.Popen(cmd, cwd=repo_root))
        else:
            result = subprocess.run(cmd, cwd=repo_root, capture_output=True, text=True)
            if result.returncode != 0:
                raise AssertionError(
                    f"trigger step failed ({result.returncode}): {step['run']}\n"
                    f"stdout: {result.stdout}\nstderr: {result.stderr}"
                )


def kill_background(background_procs):
    """Terminate any tracked background processes (best effort)."""
    for proc in background_procs:
        if proc.poll() is None:
            proc.terminate()
    for proc in background_procs:
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()


def run_case(case, redfish, state_db, repo_root):
    """Execute one event_flow case. Raises AssertionError on failure.

    `redfish` is a conftest.RedfishClient; `state_db` is unused directly here
    (the trigger scripts own Redis writes) but is accepted for symmetry and
    potential future inline triggers.
    """
    background_procs = []
    subscription_location = None
    teardown = case.get("teardown")
    sub = case["subscribe"]
    expect = case["expect_event"]

    with EventListener() as listener:
        try:
            body = inject_listener_url(sub.get("body", {}), listener.url)
            resp = redfish.post(sub["endpoint"], json=body)
            expected_status = sub.get("expected_status", 201)
            assert resp.status_code == expected_status, (
                f"subscribe POST {sub['endpoint']} returned {resp.status_code} "
                f"(expected {expected_status}): {resp.text}"
            )
            subscription_location = resp.headers.get("Location")

            run_trigger_steps(case["trigger"], repo_root, background_procs)

            validators = expect["validators"]
            timeout = expect.get("timeout_seconds", 15)
            event = listener.wait_for_event(
                lambda e: event_matches(e, validators), timeout=timeout
            )
            assert event is not None, (
                f"no event matching validators arrived within {timeout}s. "
                f"validators={validators}"
            )
        finally:
            kill_background(background_procs)
            if subscription_location:
                try:
                    redfish.delete(subscription_location)
                except Exception:
                    pass
            if teardown and "run" in teardown:
                try:
                    run_trigger_steps([teardown], repo_root, [])
                except Exception:
                    pass
