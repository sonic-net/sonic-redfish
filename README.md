# sonic-redfish

SONiC Redfish implementation providing bmcweb and sonic-dbus-bridge as Debian packages.

## Table of Contents

- [Overview](#overview)
- [Quick Start](#quick-start)
- [Build System](#build-system)
- [Patch Management](#patch-management)
- [Cleanup Targets](#cleanup-targets)
- [Dependency Management](#dependency-management)
- [Configuration](#configuration)
- [Components](#components)
- [Redfish API Endpoints](#redfish-api-endpoints)
- [Troubleshooting](#troubleshooting)
- [License](#license)

## Overview

This repository contains:
- **bmcweb**: OpenBMC web server source code with SONiC-specific patches
- **sonic-dbus-bridge**: Bridge between SONiC Redis and D-Bus for bmcweb integration

Both components are built as Debian packages (`.deb`) for easy integration with SONiC.

## Quick Start

### Prerequisites

- Docker installed on your system
- Git
- sudo access (for cleaning root-owned build artifacts)

### Build

```bash
# Build all components (Docker-based, produces .deb packages)
make

# Or explicitly
make all
```

Build artifacts will be available in `target/debs/trixie/`:
- `bmcweb_1.0.0_arm64.deb`
- `bmcweb-dbg_1.0.0_arm64.deb`
- `sonic-dbus-bridge_1.0.0_arm64.deb`
- `sonic-dbus-bridge-dbgsym_1.0.0_arm64.deb`

### Build Targets

```bash
# Show all available targets
make help

# Build individual components (automatically runs clean + dependencies)
make build-bmcweb    # Runs: clean → setup-bmcweb → apply-patches → build
make build-bridge    # Runs: clean → build

# Clean build artifacts (removes build dirs, resets bmcweb source)
make clean

# Complete reset (clean + remove Docker images + full git reset)
make reset
```

### Build Options

```bash
# Use custom number of parallel jobs (default: nproc)
make SONIC_CONFIG_MAKE_JOBS=8

# Use custom output directory (default: target/debs/trixie)
make SONIC_REDFISH_TARGET=output/debs

# Build with specific bmcweb commit (default: 6926d430)
make BMCWEB_HEAD_COMMIT=abc123

# Build with custom bmcweb repository URL
make BMCWEB_REPO_URL=https://github.com/custom/bmcweb.git
```

## Build System

The build system is designed for **Debian Trixie** and uses:

1. **Docker-based builds**: All compilation happens inside a `debian:trixie` container for consistency
2. **Debian packaging**: Uses `dpkg-buildpackage` to create `.deb` packages
3. **Meson subprojects**: sdbusplus is pre-built in the Docker image; other dependencies are managed via `.wrap` files
4. **Automatic dependencies**: Build targets automatically trigger required cleanup and setup steps
5. **Patch management**: Uses a `patches/series` file to define patch order

### Build Flow

![Build Flow Chart](images/BuildFlowChart.png)

```
make all

1. Build Docker image (sonic-redfish-builder:latest)
   - Base: debian:trixie
   - Installs: build-essential, meson, debhelper, C++23 toolchain, sdbusplus

2. Setup bmcweb source
   - Auto-clone from GitHub if not present
   - Checkout to specified commit (default: 6926d430)

3. Apply patches
   - Apply patches from patches/series to bmcweb source

4. Build sonic-dbus-bridge
   - Uses pre-installed sdbusplus from Docker image
   - dpkg-buildpackage creates .deb packages

5. Build bmcweb
   - Meson downloads dependencies via .wrap files
   - dpkg-buildpackage creates .deb packages

6. Collect artifacts to target/debs/trixie/
   - bmcweb_1.0.0_arm64.deb
   - bmcweb-dbg_1.0.0_arm64.deb
   - sonic-dbus-bridge_1.0.0_arm64.deb
   - sonic-dbus-bridge-dbgsym_1.0.0_arm64.deb
   - Plus .changes, .buildinfo, .dsc files
```

### Automatic Dependencies

The build system automatically handles dependencies:

- **`build-bmcweb`**: Automatically runs `clean` → `setup-bmcweb` → `apply-patches` → build
- **`build-bridge`**: Automatically runs `clean` → build

This ensures a clean, reproducible build every time.

## Patch Management

Patches are located in `patches/` directory:
- `patches/series` - Defines patch order (lines starting with `#` are comments)
- `patches/*.patch` - Individual patch files

Current patches:
1. `0001-Integrating-bmcweb-with-SONiC-s-build-system.patch` - Adds Debian packaging


To add a new patch:
1. Make changes in bmcweb source directory
2. Generate patch: `cd bmcweb && git format-patch -1 HEAD`
3. Move patch to `patches/` directory
4. Add patch filename to `patches/series`

## Cleanup Targets

### `clean` - Remove build artifacts, reset source
- Removes: `obj-*`, `debian/`, `.deb` files, subproject builds
- Resets: bmcweb source to clean git state (so patches can be reapplied)
- Keeps: Docker images, target directory
- Use when: You want to rebuild from scratch

### `reset` - Complete cleanup
- Does everything `clean` does, plus:
- Removes: Docker images, target directory
- Resets: bmcweb to base commit with `git clean -fdx`
- Use when: You want to start completely fresh

## Dependency Management

**sdbusplus** (OpenBMC D-Bus C++ bindings) is pre-built and installed in the Docker
image (`build/Dockerfile.build`). Both bmcweb and sonic-dbus-bridge find it via
pkg-config at build time. The pinned sdbusplus and stdexec commits are configured
as `ARG` directives in the Dockerfile.

Other dependencies are managed via **Meson wrap files** (`.wrap`):

### bmcweb dependencies:
- `bmcweb/subprojects/sdbusplus.wrap` - D-Bus C++ bindings (fallback; prefers system package)
- Plus other dependencies defined in bmcweb upstream

### sonic-dbus-bridge dependencies:
- `sonic-dbus-bridge/subprojects/sdbusplus.wrap` - D-Bus C++ bindings (fallback; prefers system package)

Meson automatically downloads and builds subproject dependencies during the build process.

The Debian packages can be installed in SONiC images.

## Components

### bmcweb
- **Source**: https://github.com/openbmc/bmcweb
- **Base commit**: 6926d430 (configurable via `BMCWEB_HEAD_COMMIT`)
- **License**: Apache-2.0
- **Purpose**: Redfish API server providing standard Redfish REST API
- **Build system**: Meson + Debian packaging
- **Output**: `bmcweb_1.0.0_arm64.deb`, `bmcweb-dbg_1.0.0_arm64.deb`
- **Auto-clone**: Automatically cloned from GitHub if not present

### sonic-dbus-bridge
- **License**: Apache-2.0
- **Purpose**: Bridge SONiC Redis database to D-Bus for bmcweb integration
- **Features**:
  - Redis to D-Bus data synchronization
  - Platform inventory management
  - FRU EEPROM data export
  - User management integration
  - State management (host, chassis)
- **Build system**: Meson + Debian packaging
- **Output**: `sonic-dbus-bridge_1.0.0_arm64.deb`, `sonic-dbus-bridge-dbgsym_1.0.0_arm64.deb`
- **Configuration**: `config/config.yaml` for Redis, D-Bus, and platform settings

## Configuration

### sonic-dbus-bridge Configuration

The bridge is configured via `sonic-dbus-bridge/config/config.yaml`:

- **Redis settings**: Connection parameters for CONFIG_DB and STATE_DB
- **Platform data**: Path to platform.json and FRU EEPROM locations
- **D-Bus settings**: Service name and bus configuration
- **Update behavior**: Polling intervals and pub/sub settings
- **Logging**: Log levels and output configuration

### D-Bus Configuration Files

D-Bus security policies are defined in `sonic-dbus-bridge/dbus/`:
- `xyz.openbmc_project.Inventory.Manager.conf` - Inventory management
- `xyz.openbmc_project.ObjectMapper.conf` - Object mapper service
- `xyz.openbmc_project.State.Host.conf` - Host state management
- `xyz.openbmc_project.User.Manager.conf` - User management
- `xyz.openbmc_project.bmcweb.conf` - bmcweb service

These files are installed to `/etc/dbus-1/system.d/` during package installation.

## Redfish API Endpoints

Below are the currently supported Redfish API endpoints and their sample responses.

### FirmwareInventory Collection

```
GET /redfish/v1/UpdateService/FirmwareInventory
```

```json
{
  "@odata.id": "/redfish/v1/UpdateService/FirmwareInventory",
  "@odata.type": "#SoftwareInventoryCollection.SoftwareInventoryCollection",
  "Members": [
    {
      "@odata.id": "/redfish/v1/UpdateService/FirmwareInventory/bios"
    },
    {
      "@odata.id": "/redfish/v1/UpdateService/FirmwareInventory/bmc"
    },
    {
      "@odata.id": "/redfish/v1/UpdateService/FirmwareInventory/switch"
    }
  ],
  "Members@odata.count": 3,
  "Name": "Software Inventory Collection"
}
```

### FirmwareInventory - BIOS

```
GET /redfish/v1/UpdateService/FirmwareInventory/bios
```

```json
{
  "@odata.id": "/redfish/v1/UpdateService/FirmwareInventory/bios",
  "@odata.type": "#SoftwareInventory.v1_1_0.SoftwareInventory",
  "Description": "Other image",
  "Id": "bios",
  "Name": "Software Inventory",
  "Status": {
    "Health": "OK",
    "HealthRollup": "OK",
    "State": "Enabled"
  },
  "Updateable": false,
  "Version": "N/A"
}
```

### FirmwareInventory - BMC Firmware

```
GET /redfish/v1/UpdateService/FirmwareInventory/bmc
```

```json
{
  "@odata.id": "/redfish/v1/UpdateService/FirmwareInventory/bmc",
  "@odata.type": "#SoftwareInventory.v1_1_0.SoftwareInventory",
  "Description": "BMC image",
  "Id": "bmc",
  "Name": "Software Inventory",
  "RelatedItem": [
    {
      "@odata.id": "/redfish/v1/Managers/bmc"
    }
  ],
  "RelatedItem@odata.count": 1,
  "Status": {
    "Health": "OK",
    "HealthRollup": "OK",
    "State": "Enabled"
  },
  "Updateable": false,
  "Version": "sonic-redfish-build.0-ddbc425a4"
}
```

### FirmwareInventory - Switch

```
GET /redfish/v1/UpdateService/FirmwareInventory/switch
```

```json
{
  "@odata.id": "/redfish/v1/UpdateService/FirmwareInventory/switch",
  "@odata.type": "#SoftwareInventory.v1_1_0.SoftwareInventory",
  "Description": "Host image",
  "Id": "switch",
  "Name": "Software Inventory",
  "RelatedItem": [
    {
      "@odata.id": "/redfish/v1/Systems/system/Bios"
    }
  ],
  "RelatedItem@odata.count": 1,
  "Status": {
    "Health": "OK",
    "HealthRollup": "OK",
    "State": "Enabled"
  },
  "Updateable": false,
  "Version": "N/A"
}
```

### Service Root

```
GET /redfish/v1/
```

```json
{
  "@odata.id": "/redfish/v1",
  "@odata.type": "#ServiceRoot.v1_15_0.ServiceRoot",
  "AccountService": {
    "@odata.id": "/redfish/v1/AccountService"
  },
  "Cables": {
    "@odata.id": "/redfish/v1/Cables"
  },
  "CertificateService": {
    "@odata.id": "/redfish/v1/CertificateService"
  },
  "Chassis": {
    "@odata.id": "/redfish/v1/Chassis"
  },
  "EventService": {
    "@odata.id": "/redfish/v1/EventService"
  },
  "Id": "RootService",
  "JsonSchemas": {
    "@odata.id": "/redfish/v1/JsonSchemas"
  },
  "Links": {
    "ManagerProvidingService": {
      "@odata.id": "/redfish/v1/Managers/bmc"
    },
    "Sessions": {
      "@odata.id": "/redfish/v1/SessionService/Sessions"
    }
  },
  "Managers": {
    "@odata.id": "/redfish/v1/Managers"
  },
  "Name": "Root Service",
  "Product": "SONiCBMC",
  "ProtocolFeaturesSupported": {
    "DeepOperations": {
      "DeepPATCH": false,
      "DeepPOST": false
    },
    "ExcerptQuery": false,
    "ExpandQuery": {
      "ExpandAll": false,
      "Levels": false,
      "Links": false,
      "NoLinks": false
    },
    "FilterQuery": false,
    "OnlyMemberQuery": true,
    "SelectQuery": true
  },
  "RedfishVersion": "1.17.0",
  "Registries": {
    "@odata.id": "/redfish/v1/Registries"
  },
  "SessionService": {
    "@odata.id": "/redfish/v1/SessionService"
  },
  "Systems": {
    "@odata.id": "/redfish/v1/Systems"
  },
  "Tasks": {
    "@odata.id": "/redfish/v1/TaskService"
  },
  "TelemetryService": {
    "@odata.id": "/redfish/v1/TelemetryService"
  },
  "UUID": "00000000-0000-0000-0000-000000000000",
  "UpdateService": {
    "@odata.id": "/redfish/v1/UpdateService"
  }
}
```

### ComputerSystem.Reset - Power On

```
POST /redfish/v1/Systems/system/Actions/ComputerSystem.Reset
Content-Type: application/json

{"ResetType": "On"}
```

This action writes a host transition request to Redis STATE_DB:

```
root@sonic:/home/admin# redis-cli -n 6 HGETALL BMC_HOST_REQUEST
1) "request_id"
2) "req_1775040896_000001"
3) "requested_transition"
4) "reset-out"
5) "status"
6) "pending"
7) "timestamp"
8) "1775040896157648224"
```

### ComputerSystem.Reset - Graceful Shutdown

```
POST /redfish/v1/Systems/system/Actions/ComputerSystem.Reset
Content-Type: application/json

{"ResetType": "GracefulShutdown"}
```

Redis STATE_DB after the request:

```
root@sonic:/home/admin# redis-cli -n 6 HGETALL BMC_HOST_REQUEST
1) "request_id"
2) "req_1775041067_000002"
3) "requested_transition"
4) "reset-in"
5) "status"
6) "pending"
7) "timestamp"
8) "1775041067766120204"
```

### ComputerSystem.Reset - Power Cycle

```
POST /redfish/v1/Systems/system/Actions/ComputerSystem.Reset
Content-Type: application/json

{"ResetType": "PowerCycle"}
```

Redis STATE_DB after the request:

```
root@sonic:/home/admin# redis-cli -n 6 HGETALL BMC_HOST_REQUEST
1) "request_id"
2) "req_1775041121_000003"
3) "requested_transition"
4) "reset-cycle"
5) "status"
6) "pending"
7) "timestamp"
8) "1775041121924146637"
```

## Troubleshooting

### Build fails with "debian/changelog: No such file or directory"
Run `make reset` to completely clean the workspace, then rebuild.

### Permission denied when cleaning
The build creates root-owned files inside Docker. The Makefile uses `sudo rm` to clean them.
Make sure you have sudo access.

### Docker image build fails
Check your internet connection - the build downloads packages from Debian repositories.

### Meson subproject download fails
Check internet connection and firewall settings. Meson needs to access GitHub to download dependencies.

## License

Apache-2.0
