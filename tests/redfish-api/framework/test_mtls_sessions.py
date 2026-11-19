#######################################
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 Nexthop AI
# Copyright (C) 2024 SONiC Project
# Author: Nexthop AI
# Author: SONiC Project
# License file: sonic-redfish/LICENSE
#######################################

"""mTLS sessions must not outlive the connection that created them.

bmcweb adds one MutualTLS session to its session store for every TLS
handshake that presents a valid client certificate. The session has to leave
the store again when the connection goes away, on every teardown path;
otherwise a client that opens one connection per request grows the store, and
bmcweb's memory, without bound (sonic-net/sonic-redfish#44).

The behaviour comes from patches/0010 and nothing else checks it: the patch
series is applied with `git apply`, which verifies context and not meaning, so
a rebase that loses part of the patch still builds. These tests pin it against
the real bmcweb binary.

They are written in Python and not as JSON cases because they need a client
certificate, connections that are opened and closed on purpose, and HTTP/2.
"""

import http.client
import os
import shutil
import ssl
import subprocess
import time
from urllib.parse import urlsplit

import pytest

from data.redis_seed import REDFISH_CERTS

# bmcweb loads the CAs that may issue client certificates from this directory
# (trustStorePath in bmcweb's ssl_key_handler.hpp). It is an OpenSSL hashed
# directory and is looked up on demand, so a CA added while bmcweb is running
# is picked up by the next handshake.
TRUST_STORE = "/etc/ssl/certs/authority"

SESSIONS_URI = "/redfish/v1/SessionService/Sessions"
# Requires authentication; the service root does not
PROTECTED_URI = "/redfish/v1/Chassis"

CONNECTIONS = 25
TIMEOUT = 10  # seconds per request
# Removing the session follows the close of the socket asynchronously
SETTLE_TIMEOUT_SEC = 10.0
SETTLE_POLL_SEC = 0.1


def _openssl(*args, cwd):
    subprocess.run(
        ["openssl", *args], cwd=cwd, check=True, capture_output=True
    )


@pytest.fixture(scope="module")
def client_cert(tmp_path_factory):
    """Issue a client certificate from a throwaway CA that bmcweb trusts.

    The common name is the one the seed data lists as trusted, so the bridge
    grants the certificate access. Yields (certificate path, key path).
    """
    if shutil.which("openssl") is None:
        pytest.skip("openssl is not installed")

    work = tmp_path_factory.mktemp("mtls")
    common_name = REDFISH_CERTS["client_crt_cname"].split(",")[0].strip()

    _openssl(
        "req", "-x509", "-newkey", "ec",
        "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes",
        "-keyout", "ca.key", "-out", "ca.crt", "-days", "2",
        "-subj", "/CN=sonic-redfish-test-ca",
        "-addext", "basicConstraints=critical,CA:TRUE",
        "-addext", "keyUsage=critical,keyCertSign,cRLSign",
        cwd=work,
    )
    _openssl(
        "req", "-newkey", "ec",
        "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes",
        "-keyout", "client.key", "-out", "client.csr",
        "-subj", f"/CN={common_name}",
        cwd=work,
    )
    (work / "client.ext").write_text(
        "basicConstraints=CA:FALSE\n"
        "keyUsage=critical,digitalSignature\n"
        "extendedKeyUsage=clientAuth\n"
    )
    _openssl(
        "x509", "-req", "-in", "client.csr",
        "-CA", "ca.crt", "-CAkey", "ca.key", "-CAcreateserial",
        "-out", "client.crt", "-days", "2", "-extfile", "client.ext",
        cwd=work,
    )

    subject_hash = subprocess.run(
        ["openssl", "x509", "-in", "ca.crt", "-noout", "-subject_hash"],
        cwd=work, check=True, capture_output=True, text=True,
    ).stdout.strip()
    os.makedirs(TRUST_STORE, exist_ok=True)
    installed_ca = os.path.join(TRUST_STORE, f"{subject_hash}.0")
    shutil.copyfile(work / "ca.crt", installed_ca)

    yield str(work / "client.crt"), str(work / "client.key")

    os.remove(installed_ca)


def _session_count(redfish):
    """Number of sessions bmcweb lists, read over the basic-auth client.

    That request has a session of its own, which is the same on every call,
    so the tests only ever compare two of these counts.
    """
    resp = redfish.get(SESSIONS_URI)
    assert resp.status_code == 200, resp.text
    return resp.json()["Members@odata.count"]


def _wait_for_session_count(redfish, expected):
    deadline = time.monotonic() + SETTLE_TIMEOUT_SEC
    count = _session_count(redfish)
    while count != expected and time.monotonic() < deadline:
        time.sleep(SETTLE_POLL_SEC)
        count = _session_count(redfish)
    return count


class MtlsConnection:
    """One HTTP/1.1 connection that presents the client certificate.

    Built on http.client so that the test decides when the socket closes.
    A requests.Response keeps its socket open for as long as the object is
    referenced, which would keep the session listed and hide what is tested.
    """

    def __init__(self, redfish, client_cert):
        cert, key = client_cert
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        # bmcweb uses a self-signed server certificate in tests
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE
        context.load_cert_chain(cert, key)
        url = urlsplit(redfish.base)
        self._conn = http.client.HTTPSConnection(
            url.hostname, url.port or 443, timeout=TIMEOUT, context=context
        )

    def get(self, path, headers=None):
        """GET `path` with the certificate as the only credential."""
        self._conn.request("GET", path, headers=headers or {})
        resp = self._conn.getresponse()
        body = resp.read()
        return resp.status, body

    def close(self):
        self._conn.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def test_mtls_session_listed_while_connection_open(redfish, client_cert):
    """The session exists for as long as the connection, and no longer.

    This also shows that the other tests can see what they count: a client
    certificate does create a session, and it does appear in the collection.
    """
    baseline = _session_count(redfish)

    with MtlsConnection(redfish, client_cert) as conn:
        status, body = conn.get(PROTECTED_URI)
        assert status == 200, body
        # A second request reuses the connection, and so the session
        status, body = conn.get(PROTECTED_URI)
        assert status == 200, body
        assert _session_count(redfish) == baseline + 1

    assert _wait_for_session_count(redfish, baseline) == baseline


def test_mtls_sessions_removed_when_client_closes(redfish, client_cert):
    """One connection per request, closed by the client (the #44 scenario)."""
    baseline = _session_count(redfish)

    for _ in range(CONNECTIONS):
        with MtlsConnection(redfish, client_cert) as conn:
            status, body = conn.get(PROTECTED_URI)
            assert status == 200, body

    assert _wait_for_session_count(redfish, baseline) == baseline


def test_mtls_sessions_removed_when_server_closes(redfish, client_cert):
    """One connection per request, closed by bmcweb (`Connection: close`)."""
    baseline = _session_count(redfish)

    for _ in range(CONNECTIONS):
        with MtlsConnection(redfish, client_cert) as conn:
            status, body = conn.get(
                PROTECTED_URI, headers={"Connection": "close"}
            )
            assert status == 200, body

    assert _wait_for_session_count(redfish, baseline) == baseline


def test_mtls_sessions_removed_over_http2(redfish, client_cert):
    """One HTTP/2 connection per request.

    An HTTP/2 connection is a different object in bmcweb, which takes the
    session over from the HTTP/1 connection that did the TLS handshake.
    """
    if shutil.which("curl") is None:
        pytest.skip("curl is not installed")
    version = subprocess.run(
        ["curl", "--version"], capture_output=True, text=True, check=True
    ).stdout
    if "HTTP2" not in version:
        pytest.skip("curl was built without HTTP/2")

    cert, key = client_cert
    baseline = _session_count(redfish)

    for _ in range(CONNECTIONS):
        result = subprocess.run(
            [
                "curl", "--silent", "--insecure", "--http2",
                "--max-time", str(TIMEOUT),
                "--cert", cert, "--key", key,
                "--output", os.devnull,
                "--write-out", "%{http_code} %{http_version}",
                f"{redfish.base}{PROTECTED_URI}",
            ],
            capture_output=True, text=True,
        )
        assert result.returncode == 0, result.stderr
        assert result.stdout.split() == ["200", "2"], result.stdout

    assert _wait_for_session_count(redfish, baseline) == baseline
