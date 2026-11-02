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

"""Poll a Redfish GET until the response satisfies a list of validators.

Usage:
    assert_get.py <path> '<validators json>' [--timeout SECONDS]

The validators use the same shapes as the "validators" key of a JSON case
(exists, not_exists, equals, ...). The request is repeated until every
validator passes or the timeout elapses, which absorbs the short window in
which sonic-dbus-bridge is still applying a STATE_DB change to D-Bus. Used
as an event_flow trigger step to assert resource state between triggers.
"""

import argparse
import json
import os
import sys
import time

import requests
import urllib3

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from framework.utils import run_validators  # noqa: E402

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

BMCWEB_BASE = "https://localhost:443"
AUTH = ("bmcweb", "bmcweb")
REQUEST_TIMEOUT = 10
POLL = 1.0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    parser.add_argument("path")
    parser.add_argument("validators", help="JSON array of validators")
    parser.add_argument("--timeout", type=float, default=20.0)
    args = parser.parse_args(argv[1:])

    validators = json.loads(args.validators)
    url = BMCWEB_BASE + args.path
    deadline = time.monotonic() + args.timeout
    last = "no response yet"
    while True:
        try:
            resp = requests.get(url, auth=AUTH, verify=False, timeout=REQUEST_TIMEOUT)
            if resp.status_code == 200:
                body = resp.json()
                try:
                    run_validators(validators, body, {})
                    print(f"GET {args.path} satisfied {json.dumps(validators)}")
                    return 0
                except AssertionError as exc:
                    last = f"{exc}; body={json.dumps(body)[:600]}"
            else:
                last = f"HTTP {resp.status_code}: {resp.text[:300]}"
        except (requests.RequestException, ValueError) as exc:
            last = f"request failed: {exc}"
        if time.monotonic() >= deadline:
            print(f"GET {args.path} did not satisfy {json.dumps(validators)} within "
                  f"{args.timeout}s; last: {last}", file=sys.stderr)
            return 1
        time.sleep(POLL)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
