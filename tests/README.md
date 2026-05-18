<!--
SPDX-License-Identifier: Apache-2.0
Copyright (C) 2026 Nexthop AI
Copyright (C) 2024 SONiC Project
Author: Nexthop AI
Author: SONiC Project
License file: sonic-redfish/LICENSE
-->

# Tests

Two independent test suites live under this directory:

| Suite                                    | Type                  | Runner          |
|------------------------------------------|-----------------------|-----------------|
| [`redfish-api/`](redfish-api/)           | pytest integration    | `make test`     |
| [`unit-tests/`](unit-tests/)             | C++ gtest unit tests  | `make unit-test` |

The integration suite spins up the **whole** Redfish stack
(dbus-daemon → redis → sonic-dbus-bridge → bmcweb) inside a Docker
container and hits the live HTTPS API. The unit suite is for
pure-logic C++ classes in `sonic-dbus-bridge/` — no Redis, no D-Bus,
no network.

If a test needs Redis or D-Bus, it belongs in the integration suite.
If it doesn't, it belongs in the unit suite. There is no middle tier
on purpose.

---

## Integration tests (`redfish-api/`)

### Running

```bash
make build         # build the .deb packages first
make test          # builds sonic-redfish-test:latest, runs pytest
```

`make test` runs the whole suite under `--cap-add SYS_ADMIN` Docker (needed
because dbus-daemon binds the system bus socket). Output is piped
through [scripts/format_pytest_output.py](../scripts/format_pytest_output.py)
for an aligned `[PASS]/[FAIL]/[SKIP]` table; the container exits
non-zero if anything failed, so CI catches regressions automatically.

### Running a single test

```bash
docker run --rm --cap-add SYS_ADMIN --tmpfs /run/dbus sonic-redfish-test:latest bash -c \
    "bash tests/redfish-api/start_services.sh && \
     python3 -m pytest tests/redfish-api/ -k \"chassis\" -v"
```

Replace `"chassis"` with whatever case file or specific test name you want. You can target an entire suite (`-k "service_root"`) or a single test case (`-k "chassis::test_chassis_type"`).

### Connection details

| Setting         | Value                                  |
|-----------------|----------------------------------------|
| URL             | `https://localhost:443`                |
| Basic auth      | `bmcweb` / `bmcweb`                    |
| TLS             | self-signed; `verify=False` in client  |
| Redis CONFIG_DB | `localhost:6379` db `4`                |
| Redis STATE_DB  | `localhost:6379` db `6`                |

Defined in [redfish-api/conftest.py](redfish-api/conftest.py).

### Fixtures (all session-scoped)

| Fixture     | What it gives you                                          |
|-------------|------------------------------------------------------------|
| `redfish`   | `RedfishClient` — `requests.Session` with base URL + auth  |
| `state_db`  | `redis.StrictRedis` connected to STATE_DB (db 6)           |
| `config_db` | `redis.StrictRedis` connected to CONFIG_DB (db 4)          |

### Test data — single source of truth

Seed values live in [redfish-api/data/redis_seed.py](redfish-api/data/redis_seed.py)
as module-level constants. The framework resolves these dynamically in the JSON tests using templating:

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

Never hardcode expected values inside JSON files — the seeder and the expectation will drift. Use `{{SEED.<dict>.<key>}}` to link assertions directly to the seeded Redis data.

### Self-Contained Tests (Option B)

Tests are executed sequentially, but they do not rely on a globally shared state across files. Instead, tests use `prerequisite_calls` to fetch whatever local state they need (e.g. finding a valid Chassis URI) right before executing the main test:

```json
"prerequisite_calls": [
  {
    "endpoint": "/redfish/v1/Chassis/",
    "extract": { "CHASSIS_URI": "Members[0].@odata.id" }
  }
]
```

### What's currently covered

| File                                                                     | Scope                                                                   |
|--------------------------------------------------------------------------|-------------------------------------------------------------------------|
| [cases/service_root.json](redfish-api/cases/service_root.json)           | `/redfish/v1/`, `Product=SONiCBMC`, auth enforcement                    |
| [cases/chassis.json](redfish-api/cases/chassis.json)                     | inventory fields surfaced from CONFIG_DB `DEVICE_METADATA`              |

Skipped modules use `"skip": true` or similar logic in the framework if needed.

### Adding a new integration test

The framework is JSON-driven, meaning you don't write Python code to add tests.

1. Pick a Redfish resource you want to cover.
2. If the test needs new fixture data, add it to
   [redfish-api/data/redis_seed.py](redfish-api/data/redis_seed.py)
   as a module-level constant.
3. Create a new JSON file under `redfish-api/cases/` (or append to an existing one).
4. Define your `endpoint`, `method`, `expected_status` and validation criteria (`expected_response` for subset matching, or specific `validators`).
5. If the test needs dynamic runtime data (like a URI), add `prerequisite_calls` to fetch and store it.
6. Run `make test`. No registration step, no Makefile edit — the pytest runner (`test_runner.py`) discovers `.json` files automatically.

When asserting on D-Bus state, prefer reading back through Redfish
(end-to-end). Only drop down to the Redis fixtures (by extending Python code) when the assertion
is about state that Redfish doesn't surface (e.g. `BMC_HOST_REQUEST`
after a reset).

### Debugging

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

Common failure modes:

| Symptom                                  | First place to look                                              |
|------------------------------------------|------------------------------------------------------------------|
| `make test` exits with 401 everywhere    | bmcweb PAM user `bmcweb` not created — check `Dockerfile.test`   |
| Bridge fails to claim D-Bus name         | `sonic-dbus-bridge/dbus/*.conf` not installed under `/etc/dbus-1/system.d/` |
| bmcweb not listening on 443              | `/var/log/redfish-test/bmcweb.log` — TLS cert / port binding     |
| Tests fail with stale data               | `state_db.flushdb()` between tests, or restart the container     |
| `start_services.sh` hangs on bridge      | tail `/var/log/redfish-test/bridge.log` — usually a missing      |
|                                          | platform.json key or D-Bus policy denial                         |

---

## Unit tests (`unit-tests/`)

Small, dependency-free tests for pure-logic classes in
`sonic-dbus-bridge/`. Covers translation, fallback-precedence, and
comparison helpers — anything that does not touch Redis, D-Bus, the
filesystem, or the network. I/O paths are exercised by the integration
suite, not here.

### Running

```bash
make unit-test
```

That spins up the existing `sonic-redfish-builder:latest` container
(no extra image, no `--privileged`, no services), compiles each test,
and runs it. If the builder image predates `libgtest-dev`, the target
installs it on demand once per run.

### Running a single test ad-hoc

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

### File layout & convention

```
tests/unit-tests/
└── <module>_test.cpp        # one test file per bridge source
```

`tests/unit-tests/foo_test.cpp` is built with
`sonic-dbus-bridge/src/foo.cpp` (nothing else). The Makefile rule is a
one-liner that loops over `*_test.cpp` — no per-test configuration.

If a class needs linker deps on *other* source files, that's a signal
to either (a) keep the test in the integration suite or (b) refactor
the class to break the dep.

### Adding a new unit test

1. Pick a **pure-logic** class — no Redis client, no sdbusplus, no
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

### Debugging

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

For sanitizer runs, add `-fsanitize=address,undefined`.

### Current coverage

| Test file                                                                | Target                                                       |
|--------------------------------------------------------------------------|--------------------------------------------------------------|
| [inventory_model_test.cpp](unit-tests/inventory_model_test.cpp)          | `InventoryModelBuilder::build` precedence + `hasChanged`     |

### Non-goals

- **No mocking framework (gmock).** If a test needs mocks, the class
  is too coupled — cover it in the integration suite instead.
- **No in-process Redis / D-Bus fakes.** Same reason.
- **No coverage of orchestration classes** (`main`, `bridge_app`,
  `state_manager`, `update_engine`).

Keeping the scope tight is the point. If this starts needing a build
system of its own, we've drifted.

---

## References

- [Google Test primer](https://google.github.io/googletest/primer.html)
- [gtest assertion reference](https://google.github.io/googletest/reference/assertions.html)
- [pytest fixtures](https://docs.pytest.org/en/stable/how-to/fixtures.html)
- [Redfish specification](https://www.dmtf.org/standards/redfish)
