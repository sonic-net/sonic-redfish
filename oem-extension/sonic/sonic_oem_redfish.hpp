///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2026 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

#pragma once

#include "sonic/sonic_rack_manager.hpp"
#include "sonic/sonic_submit_alert.hpp"
#include "sonic/sonic_submit_telemetry.hpp"

// =============================================================================
// SONiC OEM Redfish extension — design notes
// =============================================================================
//
// Scope
// -----
// This OEM extension adds a single sub-resource, RackManager, under the
// standard Redfish Manager (bmc) /Oem/SONiC tree, exposing two POST actions:
//   * #SONiC.SubmitAlert      — structured alert push from a rack manager
//   * #SONiC.SubmitTelemetry  — periodic telemetry/alarm push from a rack
//                               manager
//
// Why an OEM action and not Redfish EventService / TelemetryService
// ----------------------------------------------------------------
// Both EventService and TelemetryService were considered.  The deciding
// factors against them, given the deployment shape (a chassis-internal rack
// manager pushing into a switch BMC over a private link):
//   * Direction.  EventService is BMC-as-event-source pushing to a remote
//     listener — the opposite direction of what is needed here.  Reusing it
//     as an "inbound" channel via SSE subscriptions would invert the trust
//     and addressing model.
//   * Schema fit.  TelemetryService is centered on MetricReports and
//     MetricDefinitions backed by a Sensor pull model.  The rack manager
//     pushes a fixed, flat blob of alarm/sensor scalars on a non-uniform
//     cadence; modeling each field as a Sensor would require Sensor 
//     resources and a metric definition per field for no functional
//     gain on the read side.
//   * Surface.  A pair of POST actions is the smallest surface that captures
//     the contract.  It keeps the OEM footprint reviewable line-by-line and
//     leaves room to migrate individual fields into standard Sensor /
//     MetricReport resources later without breaking the rack-manager client.
//
// Why a JSON blob over D-Bus (RackManagerReceiver) instead of typed signals
// ------------------------------------------------------------------------
// The HTTP body is forwarded verbatim, as a JSON string, over a single
// D-Bus method on the well-known name `com.sonic.RackManager`.  Alternatives
// were considered:
//   * Per-field D-Bus arguments — would require regenerating the interface
//     whenever the rack-manager contract grew a field, coupling the bridge's
//     release cadence to the rack-manager's.
//   * sd-bus signals — fire-and-forget, no per-call ack, harder to surface
//     a 5xx back to the rack manager when Redis is down.
// The JSON-blob choice:
//   * Keeps the D-Bus interface stable across schema growth.
//   * Preserves field-level evolvability (additional alarm types just appear
//     in the blob; the bridge's field_mapping table grows alongside).
//   * Gives a natural unit of work for the bridge's worker thread:
//     parse → resolve JSON paths → pipelined HSET to STATE_DB.
// The trade-off is that field-level type checking moves from D-Bus into the
// bridge (handled by field_mapping.hpp), and JSON parse cost is paid on the
// bridge side; both are acceptable at the expected request rate .
//
// Threading and back-pressure
// ---------------------------
// bmcweb's action handlers do non-blocking sd-bus method calls only.  The
// bridge serializes Redis I/O onto a dedicated worker thread with a bounded
// queue, so a slow Redis cannot stall the sd-bus dispatch loop and starve
// other bmcweb traffic.  See rack_manager_receiver.{hpp,cpp} for details.
// =============================================================================

namespace redfish
{

/**
 * @brief Register all SONiC OEM Redfish routes.
 *
 * This is the single entry point called from redfish.cpp.
 * To add a new OEM API:
 *   1. Create a new header in sonic/
 *   2. Add #include and call its requestRoutes function here.
 *
 * @param app     Crow application for registering standalone action routes
 * @param service RedfishService for registering OEM sub-routes (fragments)
 */
inline void requestRoutesSonicOem(App& app, RedfishService& service)
{
    requestRoutesSonicRackManager(service);
    requestRoutesSonicSubmitAlert(app);
    requestRoutesSonicSubmitTelemetry(app);
}

} // namespace redfish
