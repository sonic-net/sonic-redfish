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
    Format pytest output for better readability.
    Transforms raw pytest output into a clean, aligned table format. 
"""

import sys
import re
from typing import Tuple, Optional


def parse_pytest_line(line: str) -> Optional[Tuple[str, str, str, str]]:
    """Parse a pytest output line.

    Returns: (module_class_test, status, progress, full_line) or None
    """
    # Pattern: tests/test_file.py::TestClass::test_name STATUS [PROGRESS%]
    pattern = r'^(tests/.*?)\s+(PASSED|FAILED|SKIPPED|ERROR)\s+\[([^\]]+)\]$'
    match = re.match(pattern, line.strip())

    if not match:
        return None

    test_path = match.group(1)
    status = match.group(2)
    progress = match.group(3)

    return (test_path, status, progress, line)


def format_status(status: str) -> str:
    """Format status with color codes and alignment."""
    status_map = {
        'PASSED': '\033[92m[PASS]\033[0m',   # Green
        'FAILED': '\033[91m[FAIL]\033[0m',   # Red
        'SKIPPED': '\033[93m[SKIP]\033[0m',  # Yellow
        'ERROR': '\033[91m[ERR ]\033[0m',    # Red
    }
    return status_map.get(status, status.ljust(6))


def format_progress(progress: str) -> str:
    """Format progress percentage with alignment."""
    # Strip whitespace and ensure consistent width
    prog = progress.strip().rjust(5)
    return f"[{prog}]"


def shorten_test_path(test_path: str, max_width: int = 100) -> str:
    """Shorten test path if needed while keeping it readable."""
    # If it's a parameterized test, extract just the parameter ID (which is suite::case (description))
    match = re.search(r'\[(.*)\]', test_path)
    if match:
        clean_path = match.group(1)
    else:
        # Normalize path: remove 'tests/' and 'redfish-api/' prefixes for cleaner output
        clean_path = test_path
        clean_path = clean_path.replace('tests/redfish-api/', '')
        clean_path = clean_path.replace('redfish-api/', '')
        clean_path = clean_path.replace('tests/', '')

    if len(clean_path) <= max_width:
        return clean_path

    # Last resort: truncate with ellipsis
    return clean_path[:max_width-3] + '...'


def main():
    """Read pytest output from stdin and print formatted output."""

    # Unbuffer stdout for real-time progress
    sys.stdout.reconfigure(line_buffering=True)

    print("\n" + "="*100)
    print("  REDFISH INTEGRATION TEST RESULTS")
    print("="*100 + "\n")

    passed = 0
    failed = 0
    skipped = 0
    errors = 0

    in_failures_section = False
    failure_lines = []

    for line in sys.stdin:
        if "=== FAILURES ===" in line or "=== ERRORS ===" in line:
            in_failures_section = True
            failure_lines.append(line.rstrip())
            continue

        if in_failures_section:
            if "=== short test summary info ===" in line:
                in_failures_section = False
            elif re.search(r'^=+.*?(passed|failed|error).*?=+$', line.strip(), re.IGNORECASE):
                in_failures_section = False
            else:
                failure_lines.append(line.rstrip())
                continue

        parsed = parse_pytest_line(line)
        if not parsed:
            # Pass through non-test lines (summaries, errors, etc.)
            if any(re.search(fr'\b{keyword}\b', line) for keyword in ['FAILED', 'ERROR', 'passed', 'failed']) or '====' in line or '----' in line:
                print(line.rstrip())
            continue

        test_path, status, progress, _ = parsed

        # Count by status
        if status == 'PASSED':
            passed += 1
        elif status == 'FAILED':
            failed += 1
        elif status == 'SKIPPED':
            skipped += 1
        elif status == 'ERROR':
            errors += 1

        # Format and print
        status_str = format_status(status)
        progress_str = format_progress(progress)
        path_str = shorten_test_path(test_path, max_width=110)

        # Colorize only the description part in parentheses (Cyan)
        match = re.search(r'(.*?)\s*(\([^)]+\))$', path_str)
        if match:
            test_name = match.group(1)
            desc = match.group(2)
            path_str_colored = f"{test_name} \033[96m{desc}\033[0m"
        else:
            path_str_colored = path_str

        print(f"  {status_str}  {progress_str}  {path_str_colored}")
        sys.stdout.flush()  # Force immediate output
    
    # Print summary
    print("\n" + "="*100)
    if failure_lines:
        for f_line in failure_lines:
            print(f_line)
        print("="*100 + "\n")
    else:
        print("="*100 + "\n")

    # Exit with non-zero if there were failures
    sys.exit(1 if (failed > 0 or errors > 0) else 0)


if __name__ == '__main__':
    main()
