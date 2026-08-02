#!/bin/bash
#######################################
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 Nexthop AI
# Author: Nexthop AI
# License file: sonic-redfish/LICENSE
#######################################
#
# cert-watcher: monitor the HTTPS certificate directory and restart bmcweb
# whenever the certificate file is genuinely replaced (content/inode change).
#
# Events subscribed:
#   close_write  – file opened for writing was closed   (in-place update)
#   moved_to     – file atomically renamed into place   (inode change)
#   create       – new file created in the directory
#   delete       – file deleted from the directory
#
# Events intentionally NOT subscribed:
#   attrib       – chmod / chown / touch / stat all generate only IN_ATTRIB;
#                  we never watch it, so those operations are silently ignored.
#
# Configuration (via environment variables):
#   CERT_WATCH_DIR   – directory to monitor  (default: /etc/ssl/certs/https)
#   BMCWEB_RESTART_CMD – command to restart bmcweb
#                        (default: supervisorctl restart bmcweb)
#


set -euo pipefail

CERT_WATCH_DIR="${CERT_WATCH_DIR:-/etc/ssl/certs/https}"
BMCWEB_RESTART_CMD="${BMCWEB_RESTART_CMD:-supervisorctl restart bmcweb}"

if [ ! -d "$CERT_WATCH_DIR" ]; then
    echo "[cert-watcher] ERROR: directory not found: $CERT_WATCH_DIR" >&2
    exit 1
fi

echo "[cert-watcher] Watching $CERT_WATCH_DIR for certificate changes..."
echo "[cert-watcher] Restart command: $BMCWEB_RESTART_CMD"

inotifywait -m -q \
    -e close_write \
    -e moved_to \
    -e create \
    -e delete \
    --format '%:e %f' \
    "$CERT_WATCH_DIR" | while read -r event file; do
    echo "[cert-watcher] Certificate change detected: event=$event file=$file — restarting bmcweb"
    if ! eval "$BMCWEB_RESTART_CMD"; then
        echo "[cert-watcher] WARNING: restart command failed (exit $?): $BMCWEB_RESTART_CMD" >&2
    fi
done
