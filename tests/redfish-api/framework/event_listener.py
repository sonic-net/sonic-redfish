#######################################
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 Nexthop AI
# Copyright (C) 2024 SONiC Project
# Author: Nexthop AI
# Author: SONiC Project
# License file: sonic-redfish/LICENSE
#######################################

"""In-process webhook listener for Redfish push-style event subscriptions.

bmcweb POSTs event payloads to a subscription's Destination URL. EventListener
runs a threaded HTTP server, parses each POSTed JSON body, and stores it on a
thread-safe queue for the test to consume.
"""

import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from queue import Queue, Empty


class _Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length) if length else b""
        try:
            body = json.loads(raw.decode("utf-8")) if raw else {}
        except json.JSONDecodeError:
            body = {"_raw": raw.decode("utf-8", "replace")}
        self.server.event_queue.put(body)
        self.send_response(204)
        self.end_headers()

    def log_message(self, *args):  # silence default stderr logging
        pass


class EventListener:
    """Threaded webhook receiver. Use as a context manager."""

    def __init__(self, host: str = "127.0.0.1", port: int = 0):
        # port=0 -> OS assigns a free port (avoids collisions across cases).
        self._server = ThreadingHTTPServer((host, port), _Handler)
        self._server.event_queue = Queue()
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)
        bound_host, bound_port = self._server.server_address
        self.host = bound_host
        self.port = bound_port

    @property
    def url(self) -> str:
        return f"http://{self.host}:{self.port}/"

    def start(self):
        self._thread.start()
        return self

    def stop(self):
        self._server.shutdown()
        self._server.server_close()

    def wait_for_event(self, predicate, timeout: float):
        """Return the first queued event satisfying predicate(event), or None.

        Events that do not match are discarded (FIFO drain).
        """
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            try:
                event = self._server.event_queue.get(timeout=remaining)
            except Empty:
                return None
            if predicate(event):
                return event

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.stop()
        return False
