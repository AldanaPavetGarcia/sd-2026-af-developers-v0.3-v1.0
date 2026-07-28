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
     * Reconnection is only allowed when the node_id matches an existing OFFLINE node
     * and the reported metadata is identical to the original one.
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

        const bool sameMetadata = (it->second.bunkerName == record.bunkerName) &&
                                  (it->second.ipAddress == record.ipAddress) &&
                                  (it->second.capacity == record.capacity);

        if (it->second.status == NodeStatus::OFFLINE && sameMetadata)
        {
            it->second.status = NodeStatus::ONLINE;
            it->second.lastSeen = record.lastSeen;
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

    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span>
        connectionSpan; ///< Span tracking the lifetime of the session.

    SessionContext()
        : clientFd(-1)
        , connectedAt(std::chrono::steady_clock::now())
        , lastActivity(std::chrono::steady_clock::now())
        , registered(false)
        , active(true)
        , reservedForDispatch(false)
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

// ---------------------------------------------------------------------------
// Server function prototypes
// ---------------------------------------------------------------------------
void handleClientMessage(int clientSock, const std::string& clientIp, const std::string& request);
void startServer(int port, int idleTimeout);
std::string getCurrentTimestamp();

#endif