#!/usr/bin/env python3
#######################################
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 Nexthop AI
# Copyright (C) 2024 SONiC Project
# Author: Nexthop AI
# Author: SONiC Project
# License file: sonic-redfish/LICENSE
#######################################

"""Stand-in for the syslog daemon in the test container.

Listens on /dev/log and appends every datagram to a file, one per line, so
tests can read back what the services sent to syslog.

Usage: syslog_capture.py <output-file>
"""

import os
import socket
import sys

SOCKET_PATH = "/dev/log"


def main() -> int:
    out_path = sys.argv[1]
    if os.path.lexists(SOCKET_PATH):
        os.unlink(SOCKET_PATH)
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    sock.bind(SOCKET_PATH)
    os.chmod(SOCKET_PATH, 0o666)
    with open(out_path, "ab", buffering=0) as out:
        while True:
            data = sock.recv(65536)
            out.write(data.replace(b"\n", b" ").rstrip(b"\x00 ") + b"\n")


if __name__ == "__main__":
    sys.exit(main())
