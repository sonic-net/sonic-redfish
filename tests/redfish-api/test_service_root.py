"""Tests for /redfish/v1/ ServiceRoot."""

from data.redis_seed import DEVICE_METADATA


class TestServiceRoot:
    """Validate the Redfish ServiceRoot resource."""

    def test_root_returns_200(self, redfish):
        resp = redfish.get("/redfish/v1/")
        assert resp.status_code == 200

    def test_odata_type(self, redfish):
        data = redfish.get("/redfish/v1/").json()
        assert data["@odata.type"] == "#ServiceRoot.v1_15_0.ServiceRoot"

    def test_product_is_sonic_bmc(self, redfish):
        data = redfish.get("/redfish/v1/").json()
        assert data["Product"] == "SONiCBMC"

    def test_redfish_version(self, redfish):
        data = redfish.get("/redfish/v1/").json()
        assert data["RedfishVersion"] == "1.17.0"

    def test_required_collections(self, redfish):
        data = redfish.get("/redfish/v1/").json()
        for key in ("Chassis", "Systems", "Managers",
                    "AccountService", "SessionService"):
            assert key in data, f"Missing {key} in ServiceRoot"

    def test_unauthenticated_returns_401(self, redfish):
        """Requests without credentials must be rejected."""
        import requests
        resp = requests.get(
            "https://localhost:443/redfish/v1/AccountService/",
            verify=False, timeout=10,
        )
        assert resp.status_code == 401


class TestRedfishVersionEndpoint:
    """The /redfish/v1 path (no trailing slash) and /redfish must also work."""

    def test_redfish_versions(self, redfish):
        resp = redfish.get("/redfish")
        assert resp.status_code == 200

    def test_redfish_v1_no_trailing_slash(self, redfish):
        resp = redfish.get("/redfish/v1")
        assert resp.status_code == 200
