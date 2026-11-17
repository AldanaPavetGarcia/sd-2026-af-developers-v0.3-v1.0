#ifndef EOP_SERVER_HPP
#define EOP_SERVER_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <queue>
#include <random>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <opentelemetry/trace/span.h>

#include <prometheus/counter.h>
#include <prometheus/exposer.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>

#include "../lib/include/commonProtocol.hpp"
#include "../lib/include/eventAuditLogger.hpp"
#include "../lib/include/systemTimeValidator.hpp"

// ---------------------------------------------------------------------------
// Port / timeout defaults (keep in one place so tests can override them)
// ---------------------------------------------------------------------------
#ifndef EOP_PORT
#define EOP_PORT 9090
#endif
#ifndef EOP_IDLE_TIMEOUT
#define EOP_IDLE_TIMEOUT 30
#endif
#ifndef EOP_METRICS_PORT
#define EOP_METRICS_PORT 9100 ///< Default port for the Prometheus `/metrics` HTTP exposer (US-205 / ADR-009 support).
#endif

#define EOP_BUFFER_SIZE       1024
#define EOP_SMALL_BUFFER_SIZE 64

/**
 * @brief Represents the current liveness state of a registered node.
 */
enum class NodeStatus
{
    ONLINE,
    OFFLINE
};

/**
 * @brief Result of attempting to register or reconnect a node.
 */
enum class RegisterNodeResult
{
    NEW_NODE,
    RECONNECTED_NODE,
    DUPLICATE_ONLINE_NODE
};

/**
 * @brief Metadata for a single registered bunker node.
 *
 * Populated from a REGISTER message (AC1).
 * Persisted in memory for the lifetime of the server process (AC4).
 */
struct NodeRecord
{
    std::string nodeId;                             ///< Unique string identifier sent by the client.
    std::string bunkerName;                         ///< Human-readable bunker name.
    std::string ipAddress;                          ///< IP address reported by the client.
    int capacity;                                   ///< Capacity value reported by the client.
    NodeStatus status;                              ///< Current node liveness state.
    std::chrono::system_clock::time_point lastSeen; ///< Last known activity timestamp.
    std::string sessionId;                          ///< Session currently associated with the node.

    NodeRecord()
        : capacity(0)
        , status(NodeStatus::OFFLINE)
        , lastSeen(std::chrono::system_clock::now())
    {
    }
};

/**
 * @brief Thread-safe, in-memory registry of all registered nodes.
 *
 * The registry survives client disconnections (AC4).
 */
struct NodeRegistry
{
    mutable std::mutex registryMutex;
    std::unordered_map<std::string, NodeRecord> nodesMap;

    NodeRegistry() {}
    ~NodeRegistry() {}

    /**
     * @brief Attempt to register a new node.
     * @return true on success, false when node_id already exists (AC3).
     */
    bool registerNode(const NodeRecord& record)
    {
        std::lock_guard<std::mutex> lock(registryMutex);
        if (nodesMap.count(record.nodeId) > 0)
        {
            return false; // duplicate
        }
        nodesMap.emplace(record.nodeId, record);
        return true;
    }

    /**
     * @brief Registers a node or reactivates it if it already exists in OFFLINE state.
     *
     * Reconnection is allowed when the node_id matches an existing OFFLINE node and
     * bunkerName/capacity are unchanged. ipAddress is deliberately NOT part of that
     * check — per ADR-018's Server Logic rule ("the session.id and IP address are
     * updated"), a reconnecting node's IP is expected to change (e.g. a Kubernetes
     * Pod restart always gets a fresh IP) and is refreshed on the record, not
     * compared against the previous one.
     *
     * @param record Candidate node record.
     * @return NEW_NODE if inserted for the first time,
     *         RECONNECTED_NODE if an existing OFFLINE node was reactivated,
     *         DUPLICATE_ONLINE_NODE if the node already exists and must be rejected.
     */
    RegisterNodeResult registerOrReconnectNode(const NodeRecord& record)
    {
        std::lock_guard<std::mutex> lock(registryMutex);
        auto it = nodesMap.find(record.nodeId);

        if (it == nodesMap.end())
        {
            nodesMap.emplace(record.nodeId, record);
            return RegisterNodeResult::NEW_NODE;
        }

        const bool sameMetadata =
            (it->second.bunkerName == record.bunkerName) && (it->second.capacity == record.capacity);

        if (it->second.status == NodeStatus::OFFLINE && sameMetadata)
        {
            it->second.status = NodeStatus::ONLINE;
            it->second.lastSeen = record.lastSeen;
            it->second.ipAddress = record.ipAddress; // ADR-018: updated, not matched
            return RegisterNodeResult::RECONNECTED_NODE;
        }

        return RegisterNodeResult::DUPLICATE_ONLINE_NODE;
    }

    /**
     * @brief Look up a node by its node_id.
     * @param queryId   The unique id to search for.
     * @param[out] out  Filled with the record when found.
     * @return true if found, false otherwise.
     */
    bool queryNode(const std::string& queryId, NodeRecord& out) const
    {
        std::lock_guard<std::mutex> lock(registryMutex);
        auto it = nodesMap.find(queryId);
        if (it == nodesMap.end())
        {
            // LCOV_EXCL_START
            return false;
            // LCOV_EXCL_STOP
        }
        out = it->second;
        return true;
    }

    /**
     * @brief Return all registered nodes (AC7).
     */
    std::vector<NodeRecord> listNodes() const
    {
        std::lock_guard<std::mutex> lock(registryMutex);
        std::vector<NodeRecord> result;
        result.reserve(nodesMap.size());
        for (const auto& kv : nodesMap)
        {
            result.push_back(kv.second);
        }
        return result;
    }
};

/**
 * @brief Represents an active TCP session handled by the server.
 */
struct SessionContext
{
    int clientFd;                                       ///< Socket file descriptor for the client connection.
    std::string sessionId;                              ///< Unique session identifier assigned by the server.
    std::string nodeId;                                 ///< Node associated with this session after registration.
    std::string clientIp;                               ///< Remote client IP address.
    std::chrono::steady_clock::time_point connectedAt;  ///< Time when the TCP session was accepted.
    std::chrono::steady_clock::time_point lastActivity; ///< Last activity time for heartbeat/timeout checks.
    bool registered;                                    ///< True if the client has completed REGISTER successfully.
    bool active;                                        ///< True while the session is considered alive.
    bool reservedForDispatch;                           ///< True when the session is reserved for server-side dispatch.
    bool draining; ///< Peer closed with a last command pending: kept open until it is answered.

    /**
     * @brief Bytes received from this client that do not yet form a complete `\n`-terminated message.
     *
     * @details ADR-003 frames messages by a trailing `\n`, but a single `recv()` returns whatever
     * happens to have arrived: a message larger than the read buffer, or one delayed mid-flight,
     * comes back split across several calls. Anything left after the last `\n` in a given read is
     * a partial message, so it is accumulated here and prepended to the next read instead of being
     * dispatched. Only ever holds an incomplete tail — it is emptied as soon as its content
     * completes a line.
     */
    std::string pendingInput;

    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span>
        connectionSpan; ///< Span tracking the lifetime of the session.

    SessionContext()
        : clientFd(-1)
        , connectedAt(std::chrono::steady_clock::now())
        , lastActivity(std::chrono::steady_clock::now())
        , registered(false)
        , active(true)
        , reservedForDispatch(false)
        , draining(false)
    {
    }
};

// ---------------------------------------------------------------------------
// Server-wide context
// ---------------------------------------------------------------------------

/**
 * @brief Represents an incoming message from a client to be processed by a worker.
 */
struct ClientMessage
{
    int clientFd;
    std::string clientIp;
    std::string message;
    /// The peer already closed: the worker closes the session after answering this message, so
    /// the fd cannot be reused by a new client before the answer is written.
    bool closeAfter = false;
};

/**
 * @brief Context for managing the server's TCP session state.
 */
struct ServerContext
{
    std::atomic<int> activeConnections; ///< Number of currently active TCP connections.
    std::atomic<bool> running;          ///< Controls graceful shutdown and monitor loops.

    std::mutex sessionsMutex;                               ///< Protects access to activeSessions.
    std::unordered_map<int, SessionContext> activeSessions; ///< Active sessions indexed by client socket fd.

    std::mutex hpcDispatchMutex; ///< Serializes ANALYZE_GRAPH dispatches to the HPC Engine.

    NodeRegistry registry; ///< In-memory node registry (AC4).

    std::atomic<int> maxClients;               ///< Maximum allowed simultaneous clients (EOP_MAX_CLIENTS).
    std::atomic<int> heartbeatIntervalSeconds; ///< Heartbeat interval configuration (EOP_HEARTBEAT_INTERVAL).
    std::atomic<int> idleTimeoutSeconds;       ///< Idle timeout configuration (EOP_IDLE_TIMEOUT).

    std::queue<ClientMessage> messageQueue; ///< Queue of client messages for the Thread Pool.
    std::mutex queueMutex;                  ///< Protects access to messageQueue.
    std::condition_variable queueCv;        ///< Notifies worker threads of new messages.

    ServerContext()
        : activeConnections(0)
        , running(true)
        , maxClients(100)
        , heartbeatIntervalSeconds(5)
        , idleTimeoutSeconds(EOP_IDLE_TIMEOUT)
    {
    }

    // Deleted copy/move to prevent accidental copies of atomic members
    ServerContext(const ServerContext&) = delete;
    ServerContext& operator=(const ServerContext&) = delete;

    ~ServerContext() {}
};

extern ServerContext serverContext;

/**
 * @brief Holds the process-wide Prometheus metrics state (US-205 / ADR-009 support).
 *
 * @details Mirrors the single-global/`extern` pattern already used by ServerContext and the
 * OpenTelemetry TracerProvider: one instance for the whole process, defined in eopServer.cpp,
 * populated by InitMetrics() and torn down by ShutdownMetrics(). The Exposer owns the
 * `/metrics` HTTP listener socket (RAII: bound in InitMetrics(), released when the
 * `unique_ptr` is reset in ShutdownMetrics() or on process teardown). Family pointers are
 * non-owning views into `registry`, valid for as long as `registry` is alive.
 *
 * @invariant `exposer`, `requestsTotalFamily`, and `requestDurationFamily` are only non-null
 * after a successful InitMetrics() call in a non-TEST_MODE build; RecordRequestMetric() must
 * tolerate them being null (TEST_MODE builds never call InitMetrics(), so metric recording in
 * unit tests goes through RecordRequestMetric() directly against a test-local registry, not
 * this global).
 *
 * `mutex` protects `registry`/`exposer`/the two Family pointers against concurrent
 * InitMetrics()/ShutdownMetrics()/RecordRequestMetric() calls — required in production only in
 * the sense that startServer() could theoretically run more than once in a process (it does in
 * this codebase's own test suite, where one fixture's server is still live while another test
 * calls startServer() again), and without it a concurrent RecordRequestMetric() could read a
 * Family pointer into a Registry that InitMetrics() just replaced/destroyed.
 */
struct MetricsRegistry
{
    mutable std::mutex mutex;                       ///< Protects the members below.
    std::shared_ptr<prometheus::Registry> registry; ///< Owns the metric families below.
    std::unique_ptr<prometheus::Exposer> exposer;   ///< RAII owner of the `/metrics` HTTP listener.
    prometheus::Family<prometheus::Counter>* requestsTotalFamily = nullptr; ///< `eop_requests_total` family.
    prometheus::Family<prometheus::Histogram>* requestDurationFamily =
        nullptr; ///< `eop_request_duration_seconds` family.
};

extern MetricsRegistry metricsRegistry;

// ---------------------------------------------------------------------------
// Server function prototypes
// ---------------------------------------------------------------------------
void handleClientMessage(int clientSock, const std::string& clientIp, const std::string& request);
void startServer(int port, int idleTimeout);
std::string getCurrentTimestamp();

/**
 * @brief Initializes the Prometheus metrics registry and starts the `/metrics` HTTP exposer.
 *
 * @details Mirrors InitTracer()'s self-contained pattern: reads `EOP_METRICS_PORT` from the
 * environment, falling back to the ::EOP_METRICS_PORT compile-time default (no config.env
 * tier — like the OTel tracer's own settings, this is read directly from the environment,
 * since both container runtimes this server ships on (Docker Compose, Kubernetes) already
 * inject configuration as real environment variables). Registers `eop_requests_total`
 * (Counter) and `eop_request_duration_seconds` (Histogram) on ::metricsRegistry's registry.
 * Compiled to a no-op under `TEST_MODE`, so it is safe to call unconditionally from
 * startServer() regardless of caller (production `main()` or a GTest fixture).
 *
 * @pre Called at most once per process — a second call would attempt to bind the exposer's
 * listening socket twice and fail.
 * @post On success (non-TEST_MODE), ::metricsRegistry's `registry`, `exposer`,
 * `requestsTotalFamily`, and `requestDurationFamily` are all non-null.
 */
void InitMetrics();

/**
 * @brief Stops the Prometheus `/metrics` HTTP exposer and releases ::metricsRegistry's state.
 *
 * @details Mirrors ShutdownTracer()'s pattern; a no-op under `TEST_MODE`. Safe to call even if
 * InitMetrics() was never called (e.g. it failed to bind the listener).
 */
void ShutdownMetrics();

/**
 * @brief Records one completed request's outcome and duration in the metrics registry.
 *
 * @details Increments `eop_requests_total{command,result,status,service="eop-server"}` and observes
 * `eop_request_duration_seconds{command,service="eop-server"}`. Unlike OpenTelemetry span
 * recording in workerLoop() (which is skipped for `HEARTBEAT` and `unknown_operation` via
 * `shouldTrace`), this must be called for every dispatched command, including those two —
 * metrics need full request-volume counting, not a sampled trace view. A no-op if
 * ::metricsRegistry has not been initialized (`requestsTotalFamily`/`requestDurationFamily`
 * null), so it is always safe to call from workerLoop() even under `TEST_MODE`.
 *
 * @param command         Command name recorded in the `command` label (e.g. `"REGISTER"`,
 *                         `"HEARTBEAT"`, `"unknown_operation"`).
 * @param status           Outcome recorded in the `status` label. Only `"OK"` and `"ERROR"`
 *                         are meaningful today — mirrors the contract of
 *                         `g_logContext.result` (see eventAuditLogger.hpp).
 * @param durationSeconds Wall-clock duration of the request, in seconds.
 */
void RecordRequestMetric(const std::string& command, const std::string& status, double durationSeconds);

#endif
