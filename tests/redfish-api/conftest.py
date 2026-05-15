"""Shared pytest fixtures for Redfish integration tests.

These fixtures assume the test services (dbus-daemon, redis, sonic-dbus-bridge,
bmcweb) are already running -- started by tests/start_services.sh before pytest
is invoked.
"""

import urllib3
import pytest
import redis
import requests

# This absolute import from the 'data' package works because pytest.ini
# configures testpaths='tests/redfish-api', adding it to sys.path during test runs.
from data.redis_seed import seed

# Suppress TLS warnings -- bmcweb uses a self-signed certificate in tests
urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

BMCWEB_BASE = "https://localhost:443"
AUTH = ("bmcweb", "bmcweb")
TIMEOUT = 10  # seconds per request


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------


class RedfishClient:
    """Thin wrapper around requests.Session with base URL and auth baked in."""

    def __init__(self, base_url: str, auth: tuple[str, str]):
        self.base = base_url.rstrip("/")
        self.session = requests.Session()
        self.session.auth = auth
        self.session.verify = False  # self-signed cert

    # -- convenience verbs ---------------------------------------------------

    def get(self, path: str, **kwargs):
        return self.session.get(
            f"{self.base}{path}", timeout=TIMEOUT, **kwargs
        )

    def post(self, path: str, **kwargs):
        return self.session.post(
            f"{self.base}{path}", timeout=TIMEOUT, **kwargs
        )

    def patch(self, path: str, **kwargs):
        return self.session.patch(
            f"{self.base}{path}", timeout=TIMEOUT, **kwargs
        )

    def delete(self, path: str, **kwargs):
        return self.session.delete(
            f"{self.base}{path}", timeout=TIMEOUT, **kwargs
        )


@pytest.fixture(scope="function", autouse=True)
def reset_redis_state():
    """Function-scoped fixture to reset Redis state before every test.

    Ensures tests start with a clean, known-good Redis state and prevents
    ordering coupling when tests mutate CONFIG_DB or STATE_DB.
    """
    seed()


@pytest.fixture(scope="session")
def redfish():
    """Session-scoped Redfish HTTP client."""
    return RedfishClient(BMCWEB_BASE, AUTH)


@pytest.fixture(scope="session")
def state_db():
    """Session-scoped Redis client connected to STATE_DB (db 6)."""
    return redis.StrictRedis(
        host="localhost", port=6379, db=6, decode_responses=True
    )


@pytest.fixture(scope="session")
def config_db():
    """Session-scoped Redis client connected to CONFIG_DB (db 4)."""
    return redis.StrictRedis(
        host="localhost", port=6379, db=4, decode_responses=True
    )
