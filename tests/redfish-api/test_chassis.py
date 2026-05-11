"""Tests for /redfish/v1/Chassis/ endpoints.

Validates that sonic-dbus-bridge correctly publishes chassis inventory data
from Redis CONFIG_DB → D-Bus → bmcweb Redfish.
"""

from data.redis_seed import DEVICE_METADATA


class TestChassisCollection:
    """Validate the Chassis collection resource."""

    def test_collection_returns_200(self, redfish):
        resp = redfish.get("/redfish/v1/Chassis/")
        assert resp.status_code == 200

    def test_collection_has_members(self, redfish):
        data = redfish.get("/redfish/v1/Chassis/").json()
        assert "Members" in data
        assert len(data["Members"]) >= 1

    def test_odata_type(self, redfish):
        data = redfish.get("/redfish/v1/Chassis/").json()
        assert data["@odata.type"] == \
            "#ChassisCollection.ChassisCollection"


class TestChassisInstance:
    """Validate an individual Chassis resource populated from Redis seed data.

    sonic-dbus-bridge reads DEVICE_METADATA|localhost from CONFIG_DB and
    exposes it on D-Bus as /xyz/openbmc_project/inventory/system/chassis.
    bmcweb maps this to a Chassis member.
    """

    def _get_chassis_uri(self, redfish) -> str:
        """Return the @odata.id of the first chassis member."""
        members = redfish.get("/redfish/v1/Chassis/").json()["Members"]
        return members[0]["@odata.id"]

    def test_chassis_returns_200(self, redfish):
        uri = self._get_chassis_uri(redfish)
        assert redfish.get(uri).status_code == 200

    def test_chassis_type(self, redfish):
        uri = self._get_chassis_uri(redfish)
        data = redfish.get(uri).json()
        assert data["ChassisType"] == "RackMount"

    def test_serial_number_from_redis(self, redfish):
        uri = self._get_chassis_uri(redfish)
        data = redfish.get(uri).json()
        assert data.get("SerialNumber") == DEVICE_METADATA["serial_number"]

    def test_manufacturer_from_redis(self, redfish):
        uri = self._get_chassis_uri(redfish)
        data = redfish.get(uri).json()
        assert data.get("Manufacturer") == DEVICE_METADATA["manufacturer"]

    def test_model_from_redis(self, redfish):
        uri = self._get_chassis_uri(redfish)
        data = redfish.get(uri).json()
        assert data.get("Model") == DEVICE_METADATA["model"]

    def test_part_number_from_redis(self, redfish):
        uri = self._get_chassis_uri(redfish)
        data = redfish.get(uri).json()
        assert data.get("PartNumber") == DEVICE_METADATA["part_number"]

    def test_thermal_subsystem_link(self, redfish):
        uri = self._get_chassis_uri(redfish)
        data = redfish.get(uri).json()
        assert "ThermalSubsystem" in data
