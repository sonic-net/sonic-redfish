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
`sonic-dbus-bridge`, which walks a declarative field-mapping table
(`sonic-dbus-bridge/include/field_mapping.hpp`) to persist the data as
`HSET` commands into Redis **STATE_DB** (db index 6).

<sub>[^ Back to Table of Contents](#table-of-contents)</sub>

---

## 2. Body envelopes at a glance

| Action            | Top-level key | Notes                                                  |
| ----------------- | ------------- | ------------------------------------------------------ |
| `SubmitAlert`     | `Redfish`     | Open map. alert-type-name -> `AlertEntry` or wrapper.   |
| `SubmitTelemetry` | `Alarms`      | Open map. sensor flags + scalars + deviation blocks.   |

> **Note:** bmcweb returns `400 PropertyMissing` if the expected envelope
> key is absent.

<sub>[^ Back to Table of Contents](#table-of-contents)</sub>

---

## 3. Schema structure

`SonicManager.v1_0_0.json` is the authoritative schema. Its definitions:

1. **`Manager`** - the OEM extension object embedded under `Manager.Oem.SONiC`.
2. **`RackManager`** - the OEM sub-resource (its own `@odata.id`).
3. **`SubmitAlert`** - the alert POST action (descriptor + parameters).
4. **`AlertEntry`** - the per-alert entry shape. `Severity` is required;
   `RscmPosition` is optional because the wrapped form (see below) puts it
   on the parent.
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
| `SubmitAlert` body root             | `SubmitAlert.parameters`                | yes      | n/a       | Body must contain the `Redfish` parameter.                                                             |
| `SubmitAlert` -> `Redfish`          | `SubmitAlert.parameters.Redfish`        | yes      | yes       | Container keyed by alert-type name; values are `AlertEntry` or a wrapper (e.g. `ShutdownAlert`).       |
| Each alert entry (flat or nested)   | `AlertEntry`                            | yes      | yes       | `Severity` required. `RscmPosition` lives on the entry (flat) or on the wrapper (nested).              |
| `SubmitTelemetry` body root         | `SubmitTelemetry.parameters`            | yes      | n/a       | Body must contain the `Alarms` parameter.                                                              |
| `SubmitTelemetry` -> `Alarms`       | `Alarms`                                | yes      | yes       | Open object; field set is defined by the bridge's `field_mapping.hpp`, not enumerated in this schema.  |
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

The `Redfish` envelope holds one entry per alert type. Each entry must
carry `Severity` (one of `Normal` / `Minor` / `Major` / `Critical`) and,
in this form, `RscmPosition`. Type-specific fields (`FlowRate`,
`LiquidPressure`, `InletTemperature`, ...) sit alongside.

**Request**

```json
POST /redfish/v1/Managers/bmc/Oem/SONiC/RackManager/Actions/SONiC.SubmitAlert
{
  "Redfish": {
    "Alerts": {
      "InletTemperature": 18,
      "FlowRate": 58,
      "Severity": "Minor",
      "RscmPosition": 1
    },
    "LiquidPressureDeviation": {
      "LiquidPressure": 68,
      "Severity": "Major",
      "RscmPosition": 1
    },
    "InletTemperatureDeviation": {
      "InletTemperature": 46,
      "Severity": "Critical",
      "RscmPosition": 1
    },
    "LeakDetected":  { "Severity": "Critical", "RscmPosition": 1 },
    "LeakRopeBreak": { "Severity": "Critical", "RscmPosition": 1 }
  }
}
```

**Response**

```http
HTTP/1.1 204 No Content
```

**Redis STATE_DB after the POST**

Each alert-type entry lands under its own `RSCM_ALERT|<name>` hash:

```text
HGETALL RSCM_ALERT|Alerts
  severity          = "Minor"
  rscm_position     = "1"
  inlet_temperature = "18"
  flow_rate         = "58"

HGETALL RSCM_ALERT|LiquidPressureDeviation
  severity        = "Major"
  rscm_position   = "1"
  liquid_pressure = "68"

HGETALL RSCM_ALERT|InletTemperatureDeviation
  severity          = "Critical"
  rscm_position     = "1"
  inlet_temperature = "46"

HGETALL RSCM_ALERT|LeakDetected
  severity      = "Critical"
  rscm_position = "1"

HGETALL RSCM_ALERT|LeakRopeBreak
  severity      = "Critical"
  rscm_position = "1"
```

> **Note:** the key `Alerts` *inside* `Redfish` is an alert-type name
> (the combined inlet-temperature + flow-rate alert) - it is not an
> envelope.

### 4.2. SubmitAlert - ShutdownAlert wrapped form

`ShutdownAlert` groups multiple leaf alerts under one wrapper that owns a
single `RscmPosition`. Leaf alerts in this form carry only `Severity` and
their type-specific fields.

**Request**

```json
POST /redfish/v1/Managers/bmc/Oem/SONiC/RackManager/Actions/SONiC.SubmitAlert
{
  "Redfish": {
    "ShutdownAlert": {
      "FlowRateDeviation":       { "FlowRate": 58,        "Severity": "Minor"    },
      "TempDeviation":           { "InletTemperature": 17,"Severity": "Normal"   },
      "LiquidPressureDeviation": { "LiquidPressure": 68,  "Severity": "Major"    },
      "LeakDetected":            { "Severity": "Critical" },
      "LeakRopeBreak":           { "Severity": "Critical" },
      "RscmPosition": 3
    }
  }
}
```

**Response**

```http
HTTP/1.1 204 No Content
```

**Redis STATE_DB after the POST**

The wrapper's `RscmPosition` lands on its own hash; each leaf gets a
child key:

```text
HGETALL RSCM_ALERT|ShutdownAlert
  rscm_position = "3"

HGETALL RSCM_ALERT|ShutdownAlert|FlowRateDeviation
  severity  = "Minor"
  flow_rate = "58"

HGETALL RSCM_ALERT|ShutdownAlert|TempDeviation
  severity          = "Normal"
  inlet_temperature = "17"

HGETALL RSCM_ALERT|ShutdownAlert|LiquidPressureDeviation
  severity        = "Major"
  liquid_pressure = "68"

HGETALL RSCM_ALERT|ShutdownAlert|LeakDetected
  severity = "Critical"

HGETALL RSCM_ALERT|ShutdownAlert|LeakRopeBreak
  severity = "Critical"
```

> **Note:** Readers should join the wrapper hash with each leaf hash to
> recover the full context of a wrapped alert.

### 4.3. SubmitTelemetry

**Request**

```json
POST /redfish/v1/Managers/bmc/Oem/SONiC/RackManager/Actions/SONiC.SubmitTelemetry
{
  "Alarms": {
    "EnergyValveActive":       true,
    "EnergyValvePresent":      true,
    "FlowrateSensorActive":    true,
    "PressureSensorActive":    true,
    "TemperatureSensorActive": true,
    "InletTempDeviation":      { "InletTemperature": 16.87, "Severity": "Normal"   },
    "FlowRateDeviation":       { "FlowRate": 28,            "Severity": "Normal"   },
    "LiquidPressureDeviation": { "LiquidPressure": 2,       "Severity": "Critical" },
    "LeakDetected": {
      "LeakDetected":  false,
      "LeakRopeBreak": false,
      "Severity":      "Normal"
    },
    "ThresholdConfigVersion": "03.03",
    "GlycolConcentration":    0.0,
    "ErrorState":             "0",
    "RscmPosition":           3,
    "ConfigFileCorrupted":    false
  }
}
```

**Response**

```http
HTTP/1.1 204 No Content
```

**Redis STATE_DB after the POST**

All telemetry lands in a single Redis hash, `RSCM_TELEMETRY|alarms`, with
one field per mapping-table row:

```text
HGETALL RSCM_TELEMETRY|alarms
  energy_valve_active                  = "true"
  energy_valve_present                 = "true"
  flowrate_sensor_active               = "true"
  pressure_sensor_active               = "true"
  temperature_sensor_active            = "true"
  inlet_temp_deviation_temperature     = "16.87"
  inlet_temp_deviation_severity        = "Normal"
  flow_rate_deviation_flow_rate        = "28"
  flow_rate_deviation_severity         = "Normal"
  liquid_pressure_deviation_pressure   = "2"
  liquid_pressure_deviation_severity   = "Critical"
  leak_detected                        = "false"
  leak_rope_break                      = "false"
  leak_detected_severity               = "Normal"
  threshold_config_version             = "03.03"
  glycol_concentration                 = "0"
  error_state                          = "0"
  rscm_position                        = "3"
  config_file_corrupted                = "false"
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
{ "NotRedfish": {} }

HTTP/1.1 400 Bad Request
{
  "Redfish@Message.ExtendedInfo": [
    {
      "MessageId":   "Base.1.19.PropertyMissing",
      "MessageArgs": ["Redfish"],
      "Message":     "The property Redfish is a required property and must be included in the request."
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