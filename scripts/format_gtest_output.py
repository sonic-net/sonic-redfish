#!/usr/bin/env python3
#######################################
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 Nexthop AI
# Copyright (C) 2024 SONiC Project
# Author: Nexthop AI
# Author: SONiC Project
# License file: sonic-redfish/LICENSE
#######################################

"""
    Format gtest output to match the table layout used by
    format_pytest_output.py (same colors, columns, summary box).
"""

import re
import sys

ANSI_RE = re.compile(r'\x1b\[[0-9;]*m')

RUNNING_RE = re.compile(r'^\[\s*=+\s*\]\s+Running\s+(\d+)\s+tests?\b')
OK_RE = re.compile(r'^\[\s*OK\s*\]\s+(\S+)\s+\((\d+)\s*ms\)\s*$')
FAIL_RE = re.compile(r'^\[\s*FAILED\s*\]\s+(\S+)(?:,\s+where.*)?\s+\((\d+)\s*ms\)\s*$')


def status_tag(kind: str) -> str:
    return {
        'PASS': '\033[92m[PASS]\033[0m',
        'FAIL': '\033[91m[FAIL]\033[0m',
    }[kind]


def main() -> None:
    sys.stdout.reconfigure(line_buffering=True)

    print("\n" + "=" * 100)
    print("  C++ UNIT TEST RESULTS (gtest)")
    print("=" * 100 + "\n")

    expected = 0
    completed = 0
    passed = 0
    failed = 0
    failed_names: list[str] = []

    for raw in sys.stdin:
        line = ANSI_RE.sub('', raw).rstrip('\r\n')

        m = RUNNING_RE.match(line)
        if m and expected == 0:
            expected = int(m.group(1))
            continue

        m = OK_RE.match(line)
        if m:
            completed += 1
            passed += 1
            pct = (completed * 100) // expected if expected else 0
            print(f"  {status_tag('PASS')}  [{pct:>4d}%]  {m.group(1)}")
            continue

        m = FAIL_RE.match(line)
        if m:
            completed += 1
            failed += 1
            failed_names.append(m.group(1))
            pct = (completed * 100) // expected if expected else 0
            print(f"  {status_tag('FAIL')}  [{pct:>4d}%]  {m.group(1)}")
            continue

    total = passed + failed
    print("\n" + "=" * 100)
    print(f"  SUMMARY: {total} tests")
    if passed:
        print(f"    \033[92m* {passed} passed\033[0m")
    if failed:
        print(f"    \033[91mX {failed} failed\033[0m")
        for name in failed_names:
            print(f"      - {name}")
    print("=" * 100 + "\n")

    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
