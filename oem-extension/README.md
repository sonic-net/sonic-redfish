# SONiC OEM Redfish Extension

> <- Back to [sonic-redfish root README](../README.md)

This directory contains the SONiC OEM extension for the Redfish Manager
resource, plus the JSON schemas that document the OEM contract.

## Table of contents

1. [What the extension does](#1-what-the-extension-does)
2. [Body envelopes at a glance](#2-body-envelopes-at-a-glance)
3. [Schema structure](#3-schema-structure)
   1. [Schema bindings for the POST body](#31-schema-bindings-for-the-post-body)
4. [How to prepare a POST request body](#4-how-to-prepare-a-post-request-body)
   1. [SubmitAlert - flat form](#41-submitalert---flat-form)
   2. [SubmitAlert - ShutdownAlert wrapped form](#42-submitalert---shutdownalert-wrapped-form)
   3. [SubmitTelemetry](#43-submittelemetry)
5. [Error responses](#5-error-responses)
6. [Design decisions](#6-design-decisions)
   1. [Why a JSON blob over D-Bus instead of typed arguments](#61-why-a-json-blob-over-d-bus-instead-of-typed-arguments)

---

## 1. What the extension does

A rack-manager device pushes structured alerts and periodic telemetry into
the switch BMC via two POST actions on a single OEM sub-resource:

```http
POST /redfish/v1/Managers/<id>/Oem/SONiC/RackManager/Actions/SONiC.SubmitAlert
POST /redfish/v1/Managers/<id>/Oem/SONiC/RackManager/Actions/SONiC.SubmitTelemetry
```

bmcweb validates and forwards the JSON body verbatim over D-Bus to
`sonic-dbus-bridge`, which walks a declarative sensor-rule table
(`sonic-dbus-bridge/include/field_mapping.hpp`) to persist the data as
`HSET` commands into Redis **STATE_DB** (db index 6). Each recognised
measurement / leak entry fans out into its own per-sensor key
(`RACK_MANAGER_DATA|<SensorName>` for telemetry,
`RACK_MANAGER_ALERT|<SensorName>` for alerts), matching the platform DB
schema (see the SONiC platform design doc,
[pmon-bmc-design.md - DB schema](https://github.com/sonic-net/SONiC/blob/master/doc/bmc/sonicBMC/pmon-bmc-design.md#2121-db-schema)).

<sub>[^ Back to Table of Contents](#table-of-contents)</sub>

---

## 2. Body envelopes at a glance

| Action            | Top-level envelope key | Notes                                                                       |
| ----------------- | ---------------------- | --------------------------------------------------------------------------- |
| `SubmitAlert`     | `redfish.*`            | Case-sensitive pattern (e.g. `redfish_alert_data`). Open map of alert blocks.|
| `SubmitTelemetry` | `.*Alarms.*`           | Pattern (e.g. `Alarms`, `SystemAlarms`). Open map of measurement/leak blocks.|

> **Note:** bmcweb returns `400 PropertyMissing` if no top-level key matches
> the expected envelope pattern. More than one matching envelope may be
> present; each is walked independently and their writes merge.

<sub>[^ Back to Table of Contents](#table-of-contents)</sub>

---

## 3. Schema structure

`SonicManager.v1_0_0.json` is the authoritative schema. Its definitions:

1. **`Manager`** - the OEM extension object embedded under `Manager.Oem.SONiC`.
2. **`RackManager`** - the OEM sub-resource (its own `@odata.id`).
3. **`SubmitAlert`** - the alert POST action (descriptor + parameters).
4. **`AlertEntry`** - the per-alert entry shape. `Severity` is required;
   `RscmPosition` is optional and accepted for forward compatibility, but it
   is **not** persisted under the doc-aligned DB schema.
5. **`SubmitTelemetry`** - the telemetry POST action.
6. **`Alarms`** - the telemetry payload container (open).
7. **`SonicSeverity`** - the severity enum used throughout
   (`Normal` / `Minor` / `Major` / `Critical`).

`SonicManager.json` is the unversioned alias. Clients should reference
`SonicManager.Manager`; the alias resolves to the latest versioned schema
(currently `v1_0_0`).

### 3.1. Schema bindings for the POST body

Per DMTF action idiom, each action definition splits into two blocks: the
`parameters` block describes the **POST request body** (what clients send);
the `properties` block (`target`, `title`) describes how the action is
**advertised** on GET of the parent resource. The body is validated against
`parameters`, not `properties`.

| Where in the JSON body              | Schema definition (`#/definitions/...`) | Required | Open map? | Notes                                                                                                  |
| ----------------------------------- | --------------------------------------- | -------- | --------- | ------------------------------------------------------------------------------------------------------ |
| `SubmitAlert` body root             | `SubmitAlert.parameters`                | yes      | n/a       | Body must contain a key matching the `redfish.*` envelope pattern.                                     |
| `SubmitAlert` -> `redfish.*`        | `SubmitAlert.parameters.redfish`        | yes      | yes       | Container keyed by alert-type name; values are `AlertEntry` or a wrapper (e.g. `ShutdownAlert`).       |
| Each alert entry (flat or nested)   | `AlertEntry`                            | yes      | yes       | `Severity` required (inherited from the nearest enclosing object when absent on a leaf).               |
| `SubmitTelemetry` body root         | `SubmitTelemetry.parameters`            | yes      | n/a       | Body must contain a key matching the `.*Alarms.*` envelope pattern.                                    |
| `SubmitTelemetry` -> `.*Alarms.*`   | `Alarms`                                | yes      | yes       | Open object; field set is defined by the bridge's `field_mapping.hpp`, not enumerated in this schema.  |
| `Severity` value anywhere           | `SonicSeverity`                         | -        | -         | Enum: `Normal` / `Minor` / `Major` / `Critical`.                                                       |

> **Open map** (`additionalProperties: true`) means the schema deliberately
> does not enumerate every legal key - the receiver (sonic-dbus-bridge)
> decides which keys it recognises and stores. Unknown keys are accepted
> by bmcweb and forwarded verbatim; the bridge silently drops keys not
> present in its field-mapping table.

<sub>[^ Back to Table of Contents](#table-of-contents)</sub>

---

## 4. How to prepare a POST request body

Each subsection below documents one supported body shape with three
parts: the **request** sent to bmcweb, the **response** returned, and
the resulting **Redis STATE_DB** state.

### 4.1. SubmitAlert - flat form

The `redfish.*` envelope holds one block per alert type. `Severity` (one of
`Normal` / `Minor` / `Major` / `Critical`) is inherited from the nearest
enclosing object when absent on a leaf. Measurement fields (`FlowRate`,
`LiquidPressure`, `InletTemperature`, ...) are classified by a
case-insensitive trailing-name match and fan out to canonical per-sensor
keys. Leak entries (`*Leak*`) collapse into the single `Rack_level_leak`
record. `RscmPosition` is accepted but not persisted.

**Request**

```json
POST /redfish/v1/Managers/bmc/Oem/SONiC/RackManager/Actions/SONiC.SubmitAlert
{
  "redfish_alert_data": {
    "Alerts": {
      "InletTemperature": 18,
      "FlowRate": 58,
      "Severity": "Minor"
    },
    "LiquidPressureDeviation": {
      "LiquidPressure": 68,
      "Severity": "Major"
    },
    "LeakDetected": { "Severity": "Critical" }
  }
}
```

**Response**

```http
HTTP/1.1 204 No Content
```

**Redis STATE_DB after the POST**

Each measurement / leak fans out into its own canonical
`RACK_MANAGER_ALERT|<SensorName>` hash. An alert record stores only
`severity` and `timestamp` (a leak record stores `leak` and `timestamp`):

```text
HGETALL RACK_MANAGER_ALERT|Inlet_liquid_temperature
  severity  = "Minor"
  timestamp = "2026-01-01T00:00:00.000000Z"

HGETALL RACK_MANAGER_ALERT|Inlet_liquid_flow_rate
  severity  = "Minor"
  timestamp = "2026-01-01T00:00:00.000000Z"

HGETALL RACK_MANAGER_ALERT|Inlet_liquid_pressure
  severity  = "Major"
  timestamp = "2026-01-01T00:00:00.000000Z"

HGETALL RACK_MANAGER_ALERT|Rack_level_leak
  leak      = "Critical"
  timestamp = "2026-01-01T00:00:00.000000Z"
```

> **Note:** the key `Alerts` *inside* `redfish_alert_data` is just an
> alert-type wrapper - its two numeric leaves (`InletTemperature`,
> `FlowRate`) inherit `Severity` from it and resolve to two distinct
> per-sensor keys.

### 4.2. SubmitAlert - ShutdownAlert wrapped form

`ShutdownAlert` groups multiple leaf alerts under one wrapper that owns a
single `Severity`. Leaf alerts that omit `Severity` inherit it from the
wrapper. Nesting depth and wrapper key names do not matter - each leaf is
classified by its own measurement / leak name and resolves to the same
canonical per-sensor key as the flat form.

**Request**

```json
POST /redfish/v1/Managers/bmc/Oem/SONiC/RackManager/Actions/SONiC.SubmitAlert
{
  "redfish_alert_data": {
    "ShutdownAlert": {
      "FlowRateDeviation":       { "FlowRate": 58 },
      "TempDeviation":           { "InletTemperature": 17 },
      "LiquidPressureDeviation": { "LiquidPressure": 68 },
      "LeakDetected":            { "Severity": "Critical" },
      "Severity": "Major"
    }
  }
}
```

**Response**

```http
HTTP/1.1 204 No Content
```

**Redis STATE_DB after the POST**

The wrapper's `Severity` (`Major`) is inherited by every leaf that omits
its own; `LeakDetected` keeps its `Critical`. The resulting per-sensor
keys are identical in shape to the flat form:

```text
HGETALL RACK_MANAGER_ALERT|Inlet_liquid_flow_rate
  severity  = "Major"
  timestamp = "2026-01-01T00:00:00.000000Z"

HGETALL RACK_MANAGER_ALERT|Inlet_liquid_temperature
  severity  = "Major"
  timestamp = "2026-01-01T00:00:00.000000Z"

HGETALL RACK_MANAGER_ALERT|Inlet_liquid_pressure
  severity  = "Major"
  timestamp = "2026-01-01T00:00:00.000000Z"

HGETALL RACK_MANAGER_ALERT|Rack_level_leak
  leak      = "Critical"
  timestamp = "2026-01-01T00:00:00.000000Z"
```

> **Note:** the wrapper key (`ShutdownAlert`) is not encoded in the Redis
> key - only the canonical sensor name is. Severity inheritance is the
> only context that survives.

### 4.3. SubmitTelemetry

Telemetry is walked exactly like alerts: measurement leaves are classified
by their trailing field name, leaks collapse into `Rack_level_leak`, and
`Severity` inherits from the nearest enclosing object. Unlike alerts, a
telemetry measurement record also stores the measured `value` (the
temperature record names this field `InletTemperature`) and its `unit`.
Keys not matching a sensor rule are silently dropped.

**Request**

```json
POST /redfish/v1/Managers/bmc/Oem/SONiC/RackManager/Actions/SONiC.SubmitTelemetry
{
  "Alarms": {
    "InletTempDeviation":      { "InletTemperature": 16.87, "Severity": "Normal"   },
    "FlowRateDeviation":       { "FlowRate": 28,            "Severity": "Normal"   },
    "LiquidPressureDeviation": { "LiquidPressure": 2,       "Severity": "Critical" },
    "LeakDetected":            { "Severity": "Critical" }
  }
}
```

**Response**

```http
HTTP/1.1 204 No Content
```

**Redis STATE_DB after the POST**

Each measurement / leak fans out into its own canonical
`RACK_MANAGER_DATA|<SensorName>` hash:

```text
HGETALL RACK_MANAGER_DATA|Inlet_liquid_temperature
  InletTemperature = "16.870000"
  unit             = "C"
  severity         = "Normal"
  timestamp        = "2026-01-01T00:00:00.000000Z"

HGETALL RACK_MANAGER_DATA|Inlet_liquid_flow_rate
  value     = "28"
  unit      = "gallons_per_min"
  severity  = "Normal"
  timestamp = "2026-01-01T00:00:00.000000Z"

HGETALL RACK_MANAGER_DATA|Inlet_liquid_pressure
  value     = "2"
  unit      = "psi"
  severity  = "Critical"
  timestamp = "2026-01-01T00:00:00.000000Z"

HGETALL RACK_MANAGER_DATA|Rack_level_leak
  leak      = "Critical"
  timestamp = "2026-01-01T00:00:00.000000Z"
```

<sub>[^ Back to Table of Contents](#table-of-contents)</sub>

---

## 5. Error responses

The success path is body-less (`204 No Content`). Negative paths return a
standard Redfish error envelope.

| Condition                                | Status | Body shape                                                                       |
| ---------------------------------------- | ------ | -------------------------------------------------------------------------------- |
| Empty / malformed JSON body              | 400    | `error.code == Base.1.19.MalformedJSON`                                          |
| Missing top-level envelope key           | 400    | `<EnvelopeKey>@Message.ExtendedInfo[0].MessageId == Base.1.19.PropertyMissing`   |
| GET (or any non-POST) on action target   | 405    | `error.code == Base.1.19.OperationNotAllowed`                                    |
| Unauthenticated POST                     | 401    | empty body (challenge headers only)                                              |

**Example - missing top-level key on SubmitAlert**

```http
POST .../SONiC.SubmitAlert
{ "NotRedfishAlertData": {} }

HTTP/1.1 400 Bad Request
{
  "redfish@Message.ExtendedInfo": [
    {
      "MessageId":   "Base.1.19.PropertyMissing",
      "MessageArgs": ["redfish"],
      "Message":     "The property redfish is a required property and must be included in the request."
    }
  ]
}
```

<sub>[^ Back to Table of Contents](#table-of-contents)</sub>

---

## 6. Design decisions

### 6.1. Why a JSON blob over D-Bus instead of typed arguments

The HTTP body is forwarded verbatim, as a JSON string, over a single D-Bus
method on the well-known name `com.sonic.RackManager`. Alternatives were
considered:

- **Per-field D-Bus arguments** — would require regenerating the interface
  whenever the rack-manager contract grew a field, coupling the bridge's
  release cadence to the rack-manager's.
- **sd-bus signals** — fire-and-forget, no per-call ack, harder to surface a
  5xx back to the rack manager when Redis is down.

The JSON-blob approach:
- Keeps the D-Bus interface stable across schema growth.
- Preserves field-level evolvability (additional alarm types just appear in the
  blob, the bridge's `field_mapping` table grows alongside).
- Gives a natural unit of work for the bridge's worker thread:
  parse → resolve JSON paths → pipelined HSET to STATE\_DB.

The trade-off is that field-level type checking moves from D-Bus into the
bridge (handled by `field_mapping.hpp`), and JSON parse cost is paid on the
bridge side; both are acceptable at the expected request rate.

<sub>[^ Back to Table of Contents](#table-of-contents)</sub>