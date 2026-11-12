# sonic-redfish

SONiC Redfish implementation providing bmcweb and sonic-dbus-bridge as Debian packages.

## Table of Contents

1. [Overview](#1-overview)
2. [Quick Start](#2-quick-start)
   1. [Prerequisites](#21-prerequisites)
   2. [Build](#22-build)
   3. [Build Targets](#23-build-targets)
   4. [Build Options](#24-build-options)
3. [Build System](#3-build-system)
   1. [Build Flow](#31-build-flow)
   2. [Automatic Dependencies](#32-automatic-dependencies)
4. [Patch Management](#4-patch-management)
5. [Cleanup Targets](#5-cleanup-targets)
   1. [`clean` - Remove build artifacts, reset source](#51-clean---remove-build-artifacts-reset-source)
   2. [`reset` - Complete cleanup](#52-reset---complete-cleanup)
6. [Dependency Management](#6-dependency-management)
   1. [bmcweb dependencies](#61-bmcweb-dependencies)
   2. [sonic-dbus-bridge dependencies](#62-sonic-dbus-bridge-dependencies)
7. [Components](#7-components)
   1. [bmcweb](#71-bmcweb)
   2. [sonic-dbus-bridge](#72-sonic-dbus-bridge)
8. [Configuration](#8-configuration)
   1. [sonic-dbus-bridge configuration](#81-sonic-dbus-bridge-configuration)
   2. [D-Bus configuration files](#82-d-bus-configuration-files)
9. [OEM Extension](#9-oem-extension)
10. [Testing](#10-testing)
11. [Redfish API Endpoints](#11-redfish-api-endpoints)
    1. [FirmwareInventory Collection](#111-firmwareinventory-collection)
    2. [FirmwareInventory - BIOS](#112-firmwareinventory---bios)
    3. [FirmwareInventory - BMC Firmware](#113-firmwareinventory---bmc-firmware)
    4. [FirmwareInventory - Switch](#114-firmwareinventory---switch)
    5. [Service Root](#115-service-root)
    6. [ComputerSystem.Reset - Power On](#116-computersystemreset---power-on)
    7. [ComputerSystem.Reset - Graceful Shutdown](#117-computersystemreset---graceful-shutdown)
    8. [ComputerSystem.Reset - Power Cycle](#118-computersystemreset---power-cycle)
12. [License](#12-license)

---

## 1. Overview

This repository contains:
- **bmcweb**: OpenBMC web server source code with SONiC-specific patches
- **sonic-dbus-bridge**: Bridge between SONiC Redis and D-Bus for bmcweb integration

Both components are built as Debian packages (`.deb`) for easy integration with SONiC.

<sub>[^ Back to Table of Contents](#table-of-contents)</sub>

---

## 2. Quick Start

### 2.1. Prerequisites

- Docker installed on your system
- Git
- sudo access (for cleaning root-owned build artifacts)

### 2.2. Build

```bash
# Build all components AND run unit + integration tests (Docker-based)
make

# Or explicitly
make all

# Build the .deb packages only, without running any tests
make build
```

Build artifacts will be available in `target/debs/trixie/` (`<arch>` is the
build host architecture, e.g. `arm64` or `amd64`):
- `bmcweb_1.0.0_<arch>.deb`
- `bmcweb-dbg_1.0.0_<arch>.deb`
- `sonic-dbus-bridge_1.0.0_<arch>.deb`
- `sonic-dbus-bridge-dbgsym_1.0.0_<arch>.deb`

Plus the matching `.changes`, `.buildinfo` and `.dsc` files.

### 2.3. Build Targets

```bash
# Show all available targets
make help

# Build both components, no tests
make build

# Build individual components (automatically runs clean + dependencies)
make build-bmcweb    # Runs: clean -> setup-bmcweb -> copy-oem-extension -> apply-patches -> build
make build-bridge    # Runs: clean -> build

# Run the test suites (see section 10)
make test            # Redfish API integration tests (requires a prior build)
make unit-test       # C++ gtest unit tests

# Clean build artifacts (removes build dirs and target/, resets bmcweb source)
make clean

# Complete reset (clean + delete bmcweb/ + remove Docker images)
make reset

# Remove a test container left behind by `make test NODELETE=1`
make clean-debug
```

### 2.4. Build Options

```bash
# Use custom number of parallel jobs (default: nproc)
make SONIC_CONFIG_MAKE_JOBS=8

# Use custom output directory (default: target/debs/trixie)
make SONIC_REDFISH_TARGET=output/debs

# Build with specific bmcweb commit (default: 6926d430)
make BMCWEB_HEAD_COMMIT=abc123

# Build with custom bmcweb repository URL
make BMCWEB_REPO_URL=https://github.com/custom/bmcweb.git

# Pin a different stdexec revision (default: fee4d651...)
make STDEXEC_REVISION=<sha>

# Keep the integration-test container alive for debugging
make test NODELETE=1
```

`CONFIGURED_ARCH` (default: `amd64`) is used only by the
sonic-buildimage integration targets described in section 3.

<sub>[^ Back to Table of Contents](#table-of-contents)</sub>

---

## 3. Build System

The build system is designed for **Debian Trixie** and uses:

1. **Docker-based builds**: All compilation happens inside a `debian:trixie` container for consistency
2. **Debian packaging**: Uses `dpkg-buildpackage` to create `.deb` packages
3. **Meson subprojects**: sdbusplus is pre-built in the Docker image; other dependencies are managed via `.wrap` files
4. **Automatic dependencies**: Build targets automatically trigger required cleanup and setup steps
5. **Patch management**: Uses a `patches/series` file to define patch order

### 3.1. Build Flow

![Build Flow Chart](images/BuildFlowChart.png)

```text
make all

1. Build Docker image (sonic-redfish-builder:latest)
   - Base: debian:trixie
   - Installs: build-essential, meson, debhelper, C++23 toolchain, sdbusplus

2. Setup bmcweb source
   - Auto-clone from GitHub if not present
   - Checkout to specified commit (default: 6926d430)

3. Copy OEM extension into the bmcweb source tree
   - oem-extension/sonic/ and oem-extension/schema/ (see section 9)

4. Apply patches
   - Apply patches from patches/series to bmcweb source

5. Build sonic-dbus-bridge
   - Uses pre-installed sdbusplus from Docker image
   - dpkg-buildpackage creates .deb packages

6. Build bmcweb
   - Meson downloads dependencies via .wrap files
   - dpkg-buildpackage creates .deb packages

7. Collect artifacts to target/debs/trixie/
   - bmcweb_1.0.0_<arch>.deb
   - bmcweb-dbg_1.0.0_<arch>.deb
   - sonic-dbus-bridge_1.0.0_<arch>.deb
   - sonic-dbus-bridge-dbgsym_1.0.0_<arch>.deb
   - Plus .changes, .buildinfo, .dsc files

8. Run unit tests (make unit-test), then integration tests (make test)
   - Any test failure fails the whole `make all` invocation
```

### 3.2. Automatic Dependencies

The build system automatically handles dependencies:

- **`build-bmcweb`**: Automatically runs `clean` -> `setup-bmcweb` -> `copy-oem-extension` -> `apply-patches` -> build
- **`build-bridge`**: Automatically runs `clean` -> build

This ensures a clean, reproducible build every time.

<sub>[^ Back to Table of Contents](#table-of-contents)</sub>

---

## 4. Patch Management

Patches are located in the `patches/` directory:
- `patches/series` - Defines patch order (lines starting with `#` are comments)
- `patches/*.patch` - Individual patch files

Current patches:
1. `0001-Integrating-bmcweb-with-SONiC-s-build-system.patch` - Adds Debian packaging
2. `0002-add-bmc-MAC-address-based-product-to-Redfish-service-root.patch` - Sets
   the service root `Product` to `SONiCBMC-<base MAC>` (MAC read from the bridge
   over D-Bus; falls back to `SONiCBMC` when unavailable)
3. `0003-pin-sdbusplus-to-known-good-revision.patch` - Pins the sdbusplus wrap
4. `0004-Integrate-SONiC-OEM-extension.patch` - Wires in the OEM extension (section 9)

To add a new patch:
1. Make changes in bmcweb source directory.
2. Generate patch: `cd bmcweb && git format-patch -1 HEAD`.
3. Move patch to `patches/` directory.
4. Add patch filename to `patches/series`.

<sub>[^ Back to Table of Contents](#table-of-contents)</sub>

---

## 5. Cleanup Targets

### 5.1. `clean` - Remove build artifacts, reset source

- Removes: `obj-*`, `debian/` build state, `.deb`/`.changes`/`.buildinfo`/`.dsc`
  strays at the repo root, subproject checkouts, and the `target/` directory
- Resets: bmcweb source to clean git state via `git reset --hard` + `git clean -ffdx`
  (so patches can be reapplied)
- Keeps: Docker images, the `bmcweb/` directory itself
- Needs `sudo` to reclaim ownership of root-owned files left by Docker builds
- Use when: You want to rebuild from scratch

### 5.2. `reset` - Complete cleanup

- Does everything `clean` does for `sonic-dbus-bridge` and `target/`, plus:
- Deletes: the entire `bmcweb/` source directory (re-cloned on the next build)
- Removes: the builder image (`sonic-redfish-builder:latest`) and the test image
  (`sonic-redfish-test:latest`)
- Use when: You want to start completely fresh

<sub>[^ Back to Table of Contents](#table-of-contents)</sub>

---

## 6. Dependency Management

**sdbusplus** (OpenBMC D-Bus C++ bindings) is pre-built and installed in the Docker
image (`build/Dockerfile.build`). Both bmcweb and sonic-dbus-bridge find it via
pkg-config at build time. The pinned sdbusplus and stdexec commits are configured
as `ARG` directives in the Dockerfile.

Other dependencies are managed via **Meson wrap files** (`.wrap`):

### 6.1. bmcweb dependencies

- `bmcweb/subprojects/sdbusplus.wrap` - D-Bus C++ bindings (fallback; prefers system package)
- Plus other dependencies defined in bmcweb upstream (boost, nghttp2,
  nlohmann_json, openssl, cli11, tinyxml2, zstd, gtest)

### 6.2. sonic-dbus-bridge dependencies

- `sonic-dbus-bridge/subprojects/sdbusplus.wrap` - D-Bus C++ bindings (fallback; prefers system package)

Meson automatically downloads and builds subproject dependencies during the build process.

The Debian packages can be installed in SONiC images.

<sub>[^ Back to Table of Contents](#table-of-contents)</sub>

---

## 7. Components

### 7.1. bmcweb

- **Source**: https://github.com/openbmc/bmcweb
- **Base commit**: 6926d430 (configurable via `BMCWEB_HEAD_COMMIT`)
- **License**: Apache-2.0
- **Purpose**: Redfish API server providing standard Redfish REST API
- **Build system**: Meson + Debian packaging
- **Output**: `bmcweb_1.0.0_<arch>.deb`, `bmcweb-dbg_1.0.0_<arch>.deb`
- **Auto-clone**: Automatically cloned from GitHub if not present

### 7.2. sonic-dbus-bridge

- **License**: Apache-2.0
- **Purpose**: Bridge SONiC Redis database to D-Bus for bmcweb integration
- **Features**:
  - Redis to D-Bus data synchronization
  - Platform inventory management (including the base MAC address from CONFIG_DB)
  - FRU EEPROM data export
  - User management integration
  - State management (host, chassis)
  - Rack-manager commands, alerts and telemetry persisted to STATE_DB
- **Build system**: Meson + Debian packaging
- **Output**: `sonic-dbus-bridge_1.0.0_<arch>.deb`, `sonic-dbus-bridge-dbgsym_1.0.0_<arch>.deb`
- **Configuration**: `config/config.yaml` for Redis, D-Bus, and platform settings

<sub>[^ Back to Table of Contents](#table-of-contents)</sub>

---

## 8. Configuration

### 8.1. sonic-dbus-bridge configuration

The bridge is configured via `sonic-dbus-bridge/config/config.yaml`:

- **Redis settings**: Connection parameters for CONFIG_DB and STATE_DB
- **Platform data**: Path to platform.json and FRU EEPROM locations
- **D-Bus settings**: Service name and bus configuration
- **Update behavior**: Polling intervals and pub/sub settings
- **Logging**: Log levels and output configuration

### 8.2. D-Bus configuration files

D-Bus security policies are defined in `sonic-dbus-bridge/dbus/`:

- `xyz.openbmc_project.Inventory.Manager.conf` - Inventory management
- `xyz.openbmc_project.ObjectMapper.conf` - Object mapper service
- `xyz.openbmc_project.State.Host.conf` - Host state management
- `xyz.openbmc_project.User.Manager.conf` - User management
- `xyz.openbmc_project.bmcweb.conf` - bmcweb service
- `com.sonic.RackManager.conf` - SONiC rack-manager OEM service (section 9)

These files are installed to `/etc/dbus-1/system.d/` during package installation.

<sub>[^ Back to Table of Contents](#table-of-contents)</sub>

---

## 9. OEM Extension

A SONiC-specific OEM extension is exposed under
`Manager.Oem.SONiC.RackManager` and provides two POST actions
(`SubmitAlert`, `SubmitTelemetry`) that a rack-manager device uses to
push structured alerts and periodic telemetry into the BMC. bmcweb
validates and forwards the JSON body verbatim over D-Bus to
`sonic-dbus-bridge`, which persists it as `HSET` rows in Redis STATE_DB.

The full contract - body envelopes, JSON schemas, per-action request
/ response / Redis state, and error responses - lives in:

- **[oem-extension/README.md](oem-extension/README.md)** - OEM contract,
  schema bindings, and worked POST examples.

Source layout:

- `oem-extension/sonic/` - bmcweb-side route handlers
  (`sonic_submit_alert.hpp`, `sonic_submit_telemetry.hpp`,
  `sonic_rack_manager.hpp`, `sonic_oem_redfish.hpp`).
- `oem-extension/schema/json-schema/` - authoritative
  `SonicManager.v1_0_0.json` schema (and its unversioned alias).

<sub>[^ Back to Table of Contents](#table-of-contents)</sub>

---

## 10. Testing

Two independent test suites live under [`tests/`](tests/):

| Suite                                      | Type                  | Runner            |
|--------------------------------------------|-----------------------|-------------------|
| [`tests/redfish-api/`](tests/redfish-api/) | pytest integration    | `make test`       |
| [`tests/unit-tests/`](tests/unit-tests/)   | C++ gtest unit tests  | `make unit-test`  |

The integration suite spins up the whole Redfish stack
(dbus-daemon -> redis -> sonic-dbus-bridge -> bmcweb) inside a Docker
container and hits the live HTTPS API on `https://localhost:443`. The
unit suite covers pure-logic C++ classes in `sonic-dbus-bridge/` with
no Redis, no D-Bus, and no network.

Quick start:

```bash
make test          # full integration suite (builds the test image first)
make unit-test     # C++ unit tests in the builder image
```

See **[tests/README.md](tests/README.md)** for the JSON case schema,
fixtures, debugging recipes (`NODELETE=1`), and the guide for adding a
new test case.

<sub>[^ Back to Table of Contents](#table-of-contents)</sub>

---

## 11. Redfish API Endpoints

Below are the currently supported Redfish API endpoints and their sample responses.

### 11.1. FirmwareInventory Collection

```http
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

### 11.2. FirmwareInventory - BIOS

```http
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

### 11.3. FirmwareInventory - BMC Firmware

```http
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

### 11.4. FirmwareInventory - Switch

```http
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

### 11.5. Service Root

```http
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
  "Product": "SONiCBMC-aa:18:f1:e4:27:60",
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

### 11.6. ComputerSystem.Reset - Power On

```http
POST /redfish/v1/Systems/system/Actions/ComputerSystem.Reset
Content-Type: application/json

{"ResetType": "On"}
```

This action publishes a `RACK_MANAGER_COMMAND` row to STATE_DB which
`sonic-bmcctld` (sonic-platform-daemons) consumes:

```text
root@sonic:/home/admin# redis-cli -n 6 KEYS 'RACK_MANAGER_COMMAND|*'
1) "RACK_MANAGER_COMMAND|CMD_1775040896_000001"

root@sonic:/home/admin# redis-cli -n 6 HGETALL 'RACK_MANAGER_COMMAND|CMD_1775040896_000001'
1) "command"
2) "POWER_ON"
3) "status"
4) "PENDING"
5) "result"
6) ""
7) "last_change_timestamp"
8) "2026-05-08T12:34:56.157648Z"
```

### 11.7. ComputerSystem.Reset - Graceful Shutdown

```http
POST /redfish/v1/Systems/system/Actions/ComputerSystem.Reset
Content-Type: application/json

{"ResetType": "GracefulShutdown"}
```

Redis STATE_DB after the request:

```text
root@sonic:/home/admin# redis-cli -n 6 HGETALL 'RACK_MANAGER_COMMAND|CMD_1775041067_000002'
1) "command"
2) "GRACEFUL_SHUT"
3) "status"
4) "PENDING"
5) "result"
6) ""
7) "last_change_timestamp"
8) "2026-05-08T12:37:47.766120Z"
```

### 11.8. ComputerSystem.Reset - Power Cycle

```http
POST /redfish/v1/Systems/system/Actions/ComputerSystem.Reset
Content-Type: application/json

{"ResetType": "PowerCycle"}
```

Redis STATE_DB after the request:

```text
root@sonic:/home/admin# redis-cli -n 6 HGETALL 'RACK_MANAGER_COMMAND|CMD_1775041121_000003'
1) "command"
2) "POWER_CYCLE"
3) "status"
4) "PENDING"
5) "result"
6) ""
7) "last_change_timestamp"
8) "2026-05-08T12:38:41.924146Z"
```

`sonic-bmcctld` then transitions `status` to `IN_PROGRESS` and finally
`DONE` or `FAILED`, writing a human-readable string into `result` on
failure (e.g. `CRITICAL_LEAK_PRESENT`). `sonic-dbus-bridge` subscribes to
the published `RACK_MANAGER_COMMAND|CMD_<id>` key, logs the terminal
status (`DONE` at NOTICE, `FAILED` at ERR) and releases the action slot.
A command that does not reach a terminal status within the timeout is
logged at ERR and the slot is released anyway.

Authoritative host power state is published by the daemon to
`HOST_STATE|switch-host` (`device_power_state`, `device_status`,
`last_change_timestamp`). `sonic-dbus-bridge` mirrors it onto
`xyz.openbmc_project.State.Host` (`CurrentHostState`, object `host0`),
which is what `GET /redfish/v1/Systems/system` reports as `PowerState`:

| `device_power_state`      | `device_status` | `CurrentHostState`        | Redfish `PowerState` |
|---------------------------|-----------------|---------------------------|----------------------|
| `POWERING_ON`             | (ignored)       | `TransitioningToRunning`  | `PoweringOn`         |
| `POWERING_OFF`            | (ignored)       | `TransitioningToOff`      | `PoweringOff`        |
| `GRACEFUL_SHUTTING_DOWN`  | (ignored)       | `TransitioningToOff`      | `PoweringOff`        |
| `POWER_CYCLING`           | (ignored)       | `TransitioningToRunning`  | `PoweringOn`         |
| `POWERED_ON`              | `ONLINE`        | `Running`                 | `On`                 |
| `POWERED_OFF`             | `OFFLINE`       | `Off`                     | `Off`                |
| `GRACEFUL_SHUTDOWN`       | `OFFLINE`       | `Off`                     | `Off`                |
| `POWER_CYCLE`             | `ONLINE`        | `Running`                 | `On`                 |
| any stable state          | conflicting     | follows `device_status`   |                      |
| unrecognised              | `ONLINE`/`OFFLINE` | `Running`/`Off`        |                      |
| unrecognised              | unrecognised    | unchanged                 |                      |
| row absent at startup     | -               | `Unknown`                 | omitted, `Status.State` `UnavailableOffline` |

Until `sonic-bmcctld` has published `HOST_STATE|switch-host` the bridge
reports the dedicated `Unknown` value rather than guessing, and
`GET /redfish/v1/Chassis/chassis` (whose power state is derived from the
Switch-Host) does the same. The first published row sets the real state
and emits the matching event; the unknown state itself emits none.

Only the `ResetType` values below are accepted. Any other value (e.g.
`ForceOn`, `ForceRestart`, `GracefulRestart`, `Nmi`) returns
`400 Base.ActionParameterNotSupported`:

| `ResetType`        | D-Bus transition                               | `RACK_MANAGER_COMMAND` |
|--------------------|------------------------------------------------|------------------------|
| `On`               | `xyz.openbmc_project.State.Host.Transition.On`     | `POWER_ON`         |
| `ForceOff`         | `xyz.openbmc_project.State.Chassis.Transition.Off` | `POWER_OFF`        |
| `GracefulShutdown` | `xyz.openbmc_project.State.Host.Transition.Off`    | `GRACEFUL_SHUT`    |
| `PowerCycle`       | `xyz.openbmc_project.State.Host.Transition.Reboot` | `POWER_CYCLE`      |

Each `CurrentHostState` change is also delivered to Redfish event
subscribers (`POST /redfish/v1/EventService/Subscriptions`) as a
`ResourceEvent` message (`ResourcePoweringOn`, `ResourcePoweredOn`,
`ResourcePoweringOff`, `ResourcePoweredOff`) with `OriginOfCondition`
`/redfish/v1/Systems/system`. Subscriptions may filter on
`RegistryPrefixes: ["ResourceEvent"]` or `ResourceTypes: ["ComputerSystem"]`.

Leak detectors send `Environmental.1.1.0.LeakDetected{Critical,Warning,Normal}`
and, when a sensor faults or recovers, `ResourceEvent.1.3.0.ResourceStatusChanged{Warning,OK,Critical}`,
all with resource type `LeakDetector`. A subscription filtered on
`RegistryPrefixes` needs both `Environmental` and `ResourceEvent` to receive
all of them. bmcweb accepts `MessageIds` and `OriginResources` filters, but
no SONiC event matches them (bmcweb compares `MessageIds` against
`<Registry>.<Key>` while accepting bare keys, and expects `OriginOfCondition`
as a string where these events send an object), so subscribe without them
and match on `MessageId`.

The state and `ResetType` mapping tables live in
`sonic-dbus-bridge/include/host_state_mapping.hpp` and are covered by
`tests/unit-tests/host_state_mapping_test.cpp` (`make unit-test`).

<sub>[^ Back to Table of Contents](#table-of-contents)</sub>

---

## 12. License



Apache-2.0

<sub>[^ Back to Table of Contents](#table-of-contents)</sub>
