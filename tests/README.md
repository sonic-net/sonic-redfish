<!--
SPDX-License-Identifier: Apache-2.0
Copyright (C) 2026 Nexthop AI
Copyright (C) 2024 SONiC Project
Author: Nexthop AI
Author: SONiC Project
License file: sonic-redfish/LICENSE
-->

# Tests

> <- Back to [sonic-redfish root README](../README.md)

Two independent test suites live under this directory:

| Suite                                    | Type                  | Runner          |
|------------------------------------------|-----------------------|-----------------|
| [`redfish-api/`](redfish-api/)           | pytest integration    | `make test`     |
| [`unit-tests/`](unit-tests/)             | C++ gtest unit tests  | `make unit-test` |

The integration suite spins up the **whole** Redfish stack
(dbus-daemon -> redis -> sonic-dbus-bridge -> bmcweb) inside a Docker
container and hits the live HTTPS API. The unit suite is for
pure-logic C++ classes in `sonic-dbus-bridge/` - no Redis, no D-Bus,
no network.

If a test needs Redis or D-Bus, it belongs in the integration suite.
If it doesn't, it belongs in the unit suite. There is no middle tier
on purpose.

## Table of contents

1. [Integration tests (`redfish-api/`)](#1-integration-tests-redfish-api)
   1. [Running](#11-running)
   2. [Running a single test](#12-running-a-single-test)
   3. [Connection details](#13-connection-details)
   4. [Fixtures (all session-scoped)](#14-fixtures-all-session-scoped)
   5. [Test data - single source of truth](#15-test-data---single-source-of-truth)
   6. [Self-contained tests (Option B)](#16-self-contained-tests-option-b)
   7. [JSON case schema](#17-json-case-schema)
   8. [What's currently covered](#18-whats-currently-covered)
   9. [Adding a new integration test](#19-adding-a-new-integration-test)
   10. [Debugging](#110-debugging)
2. [Unit tests (`unit-tests/`)](#2-unit-tests-unit-tests)
   1. [Running](#21-running)
   2. [Running a single test ad-hoc](#22-running-a-single-test-ad-hoc)
   3. [File layout & convention](#23-file-layout--convention)
   4. [Adding a new unit test](#24-adding-a-new-unit-test)
   5. [Debugging](#25-debugging)
   6. [Current coverage](#26-current-coverage)
3. [References](#3-references)

---

## 1. Integration tests (`redfish-api/`)

### 1.1. Running

```bash
make build         # build the .deb packages first
make test          # builds sonic-redfish-test:latest, runs pytest
```

`make test` runs the whole suite under `--cap-add SYS_ADMIN` Docker (needed
because dbus-daemon binds the system bus socket). Output is piped
through [scripts/format_pytest_output.py](../scripts/format_pytest_output.py)
for an aligned `[PASS]/[FAIL]/[SKIP]` table; the container exits
non-zero if anything failed, so CI catches regressions automatically.

### 1.2. Running a single test

```bash
docker run --rm --cap-add SYS_ADMIN --tmpfs /run/dbus sonic-redfish-test:latest bash -c \
    "bash tests/redfish-api/framework/start_services.sh && \
     python3 -m pytest tests/redfish-api/ -k \"chassis\" -v"
```

Replace `"chassis"` with whatever case file or specific test name you
want. You can target an entire suite (`-k "service_root"`) or a single
test case (`-k "chassis::test_chassis_type"`).

### 1.3. Connection details

| Setting         | Value                                  |
|-----------------|----------------------------------------|
| URL             | `https://localhost:443`                |
| Basic auth      | `bmcweb` / `bmcweb`                    |
| TLS             | self-signed; `verify=False` in client  |
| Redis CONFIG_DB | `localhost:6379` db `4`                |
| Redis STATE_DB  | `localhost:6379` db `6`                |

Defined in [redfish-api/framework/conftest.py](redfish-api/framework/conftest.py).

### 1.4. Fixtures (all session-scoped)

| Fixture     | What it gives you                                          |
|-------------|------------------------------------------------------------|
| `redfish`   | `RedfishClient` - `requests.Session` with base URL + auth  |
| `state_db`  | `redis.StrictRedis` connected to STATE_DB (db 6)           |
| `config_db` | `redis.StrictRedis` connected to CONFIG_DB (db 4)          |

### 1.5. Test data - single source of truth

Seed values live in
[redfish-api/data/redis_seed.py](redfish-api/data/redis_seed.py) as
module-level constants. The framework resolves these dynamically in
JSON tests using templating:

```json
{
  "name": "test_serial_number_from_redis",
  "method": "GET",
  "endpoint": "{{STATE.CHASSIS_URI}}",
  "expected_response": {
    "SerialNumber": "{{SEED.DEVICE_METADATA.serial_number}}"
  }
}
```

> **Note:** Never hardcode expected values inside JSON files - the
> seeder and the expectation will drift. Use `{{SEED.<dict>.<key>}}` to
> link assertions directly to the seeded Redis data.

### 1.6. Self-contained tests (Option B)

Tests are executed sequentially, but they do not rely on a globally
shared state across files. Instead, tests use `prerequisite_calls` to
fetch whatever local state they need (e.g. finding a valid Chassis URI)
right before executing the main test:

```json
"prerequisite_calls": [
  {
    "endpoint": "/redfish/v1/Chassis/",
    "extract": { "CHASSIS_URI": "Members[0].@odata.id" }
  }
]
```

### 1.7. JSON case schema

Each entry in a `cases/*.json` file is an object with the keys below.
Validation lives in [framework/validator.py](redfish-api/framework/validator.py)
and runs at pytest collection time, so a bad case fails fast.

**Required**

| Key               | Type   | Notes                                                                          |
|-------------------|--------|--------------------------------------------------------------------------------|
| `name`            | string | Unique within the file.                                                        |
| `description`     | string | Shown in the pytest summary as the cyan suffix on the test ID.                 |
| `method`          | string | `GET` / `POST` / `PATCH` / `PUT` / `DELETE`.                                   |
| `endpoint`        | string | Path; supports `{{STATE.X}}` and `{{SEED.DEVICE_METADATA.X}}` templating.      |
| `expected_status` | int    | HTTP status to assert on the response.                                         |

**Optional - request shaping**

| Key                   | Type             | Notes                                                                                       |
|-----------------------|------------------|---------------------------------------------------------------------------------------------|
| `body`                | object / array   | Request body for `POST`/`PATCH`/`PUT`. Templating applied recursively.                      |
| `auth`                | bool             | Defaults to `true`. Set `false` to send the request without basic-auth (401-path testing).  |
| `prerequisite_calls`  | array            | Pre-flight GETs whose responses populate `{{STATE.X}}` via per-call `extract` maps.         |

**Optional - response assertions**

| Key                   | Type             | Notes                                                                                       |
|-----------------------|------------------|---------------------------------------------------------------------------------------------|
| `expected_response`   | object           | Subset-match against the JSON body. **Forbidden when `expected_status` is 204.**            |
| `validators`          | array            | Path-based validators: `exists`, `not_exists`, `length_gte`, `not_equals`, `equals_state`.  |

**Optional - Redis pre/post hooks**

| Key                  | Type   | Notes                                                                                                                  |
|----------------------|--------|------------------------------------------------------------------------------------------------------------------------|
| `redis_setup`        | array  | Pre-conditions applied before the request. Currently `{"db", "action": "delete", "key" | "keys"}` is supported.        |
| `redis_validations`  | array  | Post-request DB checks: `{"db", "key", "expected_fields", "wait_for_field"?, "timeout"?, "exists"?, "not_exists"?}`.   |

`expected_fields` values are either a bare scalar (string-compared) or
a typed expectation
`{"type": "string"|"int"|"float"|"bool", "equals": ...}` or
`{"exists": true|false}`. The `wait_for_field` knob polls one field
(default 5 s, 50 ms interval) before reading the rest - useful when the
bridge drains the write asynchronously.

### 1.8. What's currently covered

| File                                                            | Scope                                                                                                                                                          |
|-----------------------------------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------|
| [cases/service_root.json](redfish-api/cases/service_root.json)  | `/redfish/v1/`, `Product=SONiCBMC`, auth enforcement                                                                                                           |
| [cases/chassis.json](redfish-api/cases/chassis.json)            | inventory fields surfaced from CONFIG_DB `DEVICE_METADATA`                                                                                                     |
| [cases/oem_manager.json](redfish-api/cases/oem_manager.json)    | OEM RackManager: action discovery, `SubmitAlert` / `SubmitTelemetry` end-to-end into STATE_DB, negative paths (400/401/405) with Redfish error-body assertions |

> **Note:** Skipped modules use `"skip": true` or similar logic in the
> framework if needed.

### 1.9. Adding a new integration test

The framework is JSON-driven; no Python is needed for the common cases
(GET assertions, POST actions, error paths, Redis end-to-end).

1. Pick a Redfish resource or action you want to cover.
2. If the test needs new seed data, add it to
   [redfish-api/data/redis_seed.py](redfish-api/data/redis_seed.py)
   as a module-level constant and reference it with
   `{{SEED.DEVICE_METADATA.<key>}}`.
3. Create a new JSON file under `redfish-api/cases/` (or append to an
   existing one). The runner discovers `*.json` automatically - no
   registration step, no Makefile edit.
4. Pick the right shape for what you're asserting:
   - **GET state**: `method`, `endpoint`, `expected_status`,
     `expected_response` (subset match) or `validators`.
   - **POST action (happy path, 204)**: add `body` for the payload and
     `redis_validations` to confirm the bridge persisted it.
     `expected_response` is forbidden on 204 - the validator rejects it.
   - **Error paths (4xx/5xx)**: assert `expected_response` against the
     Redfish error envelope (`error.code`, `@Message.ExtendedInfo[0].MessageId`,
     etc.). Use `auth: false` to exercise the 401 path.
   - **Dynamic URIs**: add `prerequisite_calls` to fetch and `extract`
     state, then reference it with `{{STATE.<key>}}`.
   - **Stale-key isolation**: use `redis_setup` (`action: delete`) to
     clear keys before the POST so a successful drain is unambiguous.
5. Run `make test`.

When asserting on D-Bus state, prefer reading back through Redfish
(end-to-end). Drop down to `redis_validations` only when the assertion
is about state that Redfish doesn't surface (e.g. the per-sensor
`RACK_MANAGER_DATA|<SensorName>` hashes after a `SubmitTelemetry` POST).

### 1.10. Debugging

`make test NODELETE=1` keeps the test container alive after pytest
exits, named `sonic-redfish-test-debug`:

```bash
make test NODELETE=1
# ... tests run, container stays up ...

docker exec -it sonic-redfish-test-debug bash
# inside the container:
curl -sku bmcweb:bmcweb https://localhost:443/redfish/v1/ | jq
tail -f /var/log/redfish-test/{bridge,bmcweb,redis,dbus}.log
busctl --system tree xyz.openbmc_project.Inventory.Manager
redis-cli -n 6 keys '*'

# when done:
make clean-debug
```

**Common failure modes**

| Symptom                                  | First place to look                                                          |
|------------------------------------------|------------------------------------------------------------------------------|
| `make test` exits with 401 everywhere    | bmcweb PAM user `bmcweb` not created - check `Dockerfile.test`               |
| Bridge fails to claim D-Bus name         | `sonic-dbus-bridge/dbus/*.conf` not installed under `/etc/dbus-1/system.d/`  |
| bmcweb not listening on 443              | `/var/log/redfish-test/bmcweb.log` - TLS cert / port binding                 |
| Tests fail with stale data               | `state_db.flushdb()` between tests, or restart the container                 |
| `start_services.sh` hangs on bridge      | `/var/log/redfish-test/bridge.log` - usually a missing platform.json key or D-Bus policy denial |

<sub>[^ Back to Table of Contents](#table-of-contents)</sub>

---

## 2. Unit tests (`unit-tests/`)

Small, dependency-free tests for pure-logic classes in
`sonic-dbus-bridge/`. Covers translation, fallback-precedence, and
comparison helpers - anything that does not touch Redis, D-Bus, the
filesystem, or the network. I/O paths are exercised by the integration
suite, not here.

### 2.1. Running

```bash
make unit-test
```

That spins up the existing `sonic-redfish-builder:latest` container
(no extra image, no `--privileged`, no services), compiles each test,
and runs it. If the builder image predates `libgtest-dev`, the target
installs it on demand once per run.

### 2.2. Running a single test ad-hoc

```bash
docker run --rm -v "$PWD:/workspace" -w /workspace \
    sonic-redfish-builder:latest bash -c \
    'g++ -std=c++20 -pthread \
        -I sonic-dbus-bridge/include \
        -I /usr/src/googletest/googletest \
        -I /usr/src/googletest/googletest/include \
        tests/unit-tests/inventory_model_test.cpp \
        sonic-dbus-bridge/src/inventory_model.cpp \
        /usr/src/googletest/googletest/src/gtest-all.cc \
        /usr/src/googletest/googletest/src/gtest_main.cc \
        -o /tmp/t && /tmp/t --gtest_filter=HasChanged.*'
```

`--gtest_filter=Suite.*` runs a single test suite;
`--gtest_filter=Suite.Case` runs a single case.

### 2.3. File layout & convention

```text
tests/unit-tests/
└── <module>_test.cpp        # one test file per bridge source
```

`tests/unit-tests/foo_test.cpp` is built with
`sonic-dbus-bridge/src/foo.cpp` (nothing else). The Makefile rule is a
one-liner that loops over `*_test.cpp` - no per-test configuration.

> **Note:** If a class needs linker deps on *other* source files, that's
> a signal to either (a) keep the test in the integration suite or
> (b) refactor the class to break the dep.

### 2.4. Adding a new unit test

1. Pick a **pure-logic** class - no Redis client, no sdbusplus, no
   file I/O. If it needs those, don't unit-test it here.
2. Create `tests/unit-tests/<module>_test.cpp` matching the filename
   of the source file under `sonic-dbus-bridge/src/<module>.cpp`.
3. Include the public header and gtest:

   ```cpp
   #include <gtest/gtest.h>
   #include "<module>.hpp"

   using namespace sonic::dbus_bridge;

   TEST(<Suite>, <Case>) {
       // arrange
       // act
       // EXPECT_EQ(...);
   }
   ```

4. Run `make unit-test`.

No extra wiring, no Makefile edits, no framework config.

### 2.5. Debugging

The unit-test target compiles with `-g -O0` so binaries are
gdb-friendly:

```bash
docker run --rm -it -v "$PWD:/workspace" -w /workspace \
    sonic-redfish-builder:latest bash
# inside the container:
g++ -std=c++20 -g -O0 -pthread \
    -I sonic-dbus-bridge/include \
    -I /usr/src/googletest/googletest \
    -I /usr/src/googletest/googletest/include \
    tests/unit-tests/inventory_model_test.cpp \
    sonic-dbus-bridge/src/inventory_model.cpp \
    /usr/src/googletest/googletest/src/gtest-all.cc \
    /usr/src/googletest/googletest/src/gtest_main.cc \
    -o /tmp/t
gdb /tmp/t
```

> **Tip:** For sanitizer runs, add `-fsanitize=address,undefined`.

### 2.6. Current coverage

| Test file                                                       | Target                                                       |
|-----------------------------------------------------------------|--------------------------------------------------------------|
| [inventory_model_test.cpp](unit-tests/inventory_model_test.cpp) | `InventoryModelBuilder::build` precedence + `hasChanged`     |

<sub>[^ Back to Table of Contents](#table-of-contents)</sub>

---

## 3. References

1. [Google Test primer](https://google.github.io/googletest/primer.html)
2. [gtest assertion reference](https://google.github.io/googletest/reference/assertions.html)
3. [pytest fixtures](https://docs.pytest.org/en/stable/how-to/fixtures.html)
4. [Redfish specification](https://www.dmtf.org/standards/redfish)

<sub>[^ Back to Table of Contents](#table-of-contents)</sub>
