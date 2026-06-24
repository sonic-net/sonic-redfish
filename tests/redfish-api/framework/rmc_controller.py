#!/usr/bin/env python3
#######################################
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 Nexthop AI
# Copyright (C) 2024 SONiC Project
# Author: Nexthop AI
# Author: SONiC Project
# License file: sonic-redfish/LICENSE
#######################################

"""Run a single event_flow case standalone, like an RMC controller would.

Usage:
    rmc_controller.py <case.json> [--index N]

Assumes the test services are already running (see
tests/redfish-api/framework/start_services.sh).
"""

import argparse
import json
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT / "tests" / "redfish-api"))

import redis  # noqa: E402
import requests  # noqa: E402
import urllib3  # noqa: E402
from framework import event_flow  # noqa: E402

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)


class _Client:
    """Minimal Redfish client mirroring conftest.RedfishClient verbs."""

    def __init__(self):
        self.base = "https://localhost:443"
        self.session = requests.Session()
        self.session.auth = ("bmcweb", "bmcweb")
        self.session.verify = False

    def post(self, path, **kw):
        return self.session.post(f"{self.base}{path}", timeout=10, **kw)

    def delete(self, path, **kw):
        return self.session.delete(f"{self.base}{path}", timeout=10, **kw)


def main(argv) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("case_file")
    parser.add_argument("--index", type=int, default=0)
    args = parser.parse_args(argv[1:])

    cases = json.loads(Path(args.case_file).read_text())
    case = cases[args.index]

    state_db = redis.StrictRedis(host="localhost", port=6379, db=6,
                                 decode_responses=True)
    print(f"[RMC] running case '{case['name']}'...")
    event_flow.run_case(case, _Client(), state_db, str(REPO_ROOT))
    print(f"[RMC] case '{case['name']}' PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
