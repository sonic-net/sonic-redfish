///////////////////////////////////////
// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Nexthop AI
// Copyright (C) 2026 SONiC Project
// Author: Nexthop AI
// Author: SONiC Project
// License file: sonic-redfish/LICENSE
///////////////////////////////////////

#pragma once

#include "field_mapping.hpp"

#include <hiredis/hiredis.h>
#include <sdbusplus/asio/object_server.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sonic::dbus_bridge
{

// D-Bus object/interface for the rack manager receiver.
// Bus name is configured via meson (RACK_MANAGER_BUSNAME = com.sonic.RackManager)
// and claimed on a dedicated connection in bridge_app. The object path and
// interface follow the bus name's `/com/sonic/...` namespace; bmcweb's OEM
// route handlers reference the identical triple via sonic_oem_constants.hpp.
constexpr const char* RACK_MANAGER_OBJ_PATH = "/com/sonic/RackManager";
constexpr const char* RACK_MANAGER_IFACE = "com.sonic.RackManager";

/**
 * @brief Receives alert / telemetry JSON from bmcweb via D-Bus and
 *        persists the data to Redis using the declarative field-mapping tables.
 *
 * D-Bus interface: com.sonic.RackManager
 *   Methods:
 *     SubmitAlert(s json)     -> b accepted
 *     SubmitTelemetry(s json) -> b accepted
 *
 * Threading model: the D-Bus method handlers parse the JSON inline and
 * enqueue a job onto an internal queue; a dedicated worker thread owns
 * the Redis connection and performs the (potentially blocking) HSETs
 * via pipelined hiredis commands.  This keeps the sdbusplus asio
 * dispatch thread free even when Redis is slow or temporarily down.
 *
 * The "accepted" reply means the JSON was well-formed and the job was
 * queued, not that it has been persisted -- alert/telemetry ingestion
 * is fire-and-forget; the producer (rack manager) is expected to retry
 * on its own cadence.  Persistence failures are logged at ERROR.
 */
class RackManagerReceiver
{
  public:
    /**
     * @param server  Object server to register the D-Bus interface on
     * @param redisHost   STATE_DB Redis host
     * @param redisPort   STATE_DB Redis port
     * @param redisDbIndex STATE_DB index (Redis SELECT target)
     */
    RackManagerReceiver(sdbusplus::asio::object_server& server,
                        const std::string& redisHost = "localhost",
                        int redisPort = 6379, int redisDbIndex = 6);
    ~RackManagerReceiver();

    // non-copyable
    RackManagerReceiver(const RackManagerReceiver&) = delete;
    RackManagerReceiver& operator=(const RackManagerReceiver&) = delete;

    /**
     * @brief Register D-Bus methods, connect to Redis, start worker.
     * @return true on success (worker started; Redis may reconnect lazily)
     */
    bool initialize();

  private:
    sdbusplus::asio::object_server& server_;
    std::shared_ptr<sdbusplus::asio::dbus_interface> iface_;

    // Redis connection parameters.  The redisContext lives exclusively
    // on the worker thread to avoid concurrent hiredis access.
    std::string redisHost_;
    int redisPort_;
    int redisDbIndex_;
    redisContext* redisCtx_;  // worker-thread-only

    // Submission queue.  Producer = D-Bus dispatch thread,
    // consumer = single worker thread.
    struct Job
    {
        // We hold raw pointers into the static mapping tables (whose
        // lifetime is the program), not copies, to keep enqueue O(1).
        const std::vector<FieldMapping>* mappings;
        // Pre-extracted (key, field, value) triples; parsing happens on
        // the dispatch thread because it is bounded and cheap, while
        // I/O is the part that must not block dispatch.
        std::vector<std::tuple<std::string, std::string, std::string>>
            writes;
    };
    std::deque<Job> queue_;
    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::atomic<bool> stopping_{false};
    std::thread worker_;

    // Hard cap on outstanding jobs to bound memory if the worker is
    // wedged.  Newer submissions are dropped (with a WARN log) once
    // exceeded -- preferable to unbounded growth under back-pressure.
    static constexpr std::size_t kMaxQueueDepth = 1024;

    bool connectRedis();   // worker-thread-only

    /**
     * @brief Parse JSON + walk the mapping table, build a Job.
     * @return std::nullopt on parse failure (caller returns false to D-Bus)
     */
    bool buildJob(const std::string& jsonStr,
                  const std::vector<FieldMapping>& mappings, Job& out);

    void workerLoop();
    void drainOne(const Job& job);
};

} // namespace sonic::dbus_bridge
