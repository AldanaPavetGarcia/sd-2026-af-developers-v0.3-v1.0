/**
 * @file nodeRegistry.hpp
 * @brief Node registry of eop-server behind an interface, with an in-memory and a Redis backend.
 *
 * @details ADR-027: with two server replicas the registry must live outside the process, so the
 * server talks to an `INodeRegistry`. `InMemoryNodeRegistry` is the v0.1 behaviour (default, unit
 * tests, docker-compose, one replica); `RedisNodeRegistry` (redisNodeRegistry.hpp) shares it
 * between replicas. `ReplicaNodeRegistry` is what the server holds: it keeps the HPC engine's own
 * node in the replica's local table, because each replica dispatches to its own sidecar engine,
 * and sends every other node to the shared backend when there is one.
 *
 * Edge devices (ADR-028) are nodes too: `upsertTelemetry` creates or updates them from the
 * ingestor's `TELEMETRY` command. They have no session: the server derives their liveness from
 * `lastSeen` when it reads them.
 *
 * Errors are return codes; nothing here throws across the module boundary.
 */

#ifndef NODE_REGISTRY_HPP
#define NODE_REGISTRY_HPP

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * @brief Liveness state of a registered node.
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
    NEW_NODE,              ///< First registration of this node_id.
    RECONNECTED_NODE,      ///< An OFFLINE node (or one whose owner replica is gone) came back.
    DUPLICATE_ONLINE_NODE, ///< The node is ONLINE on a live session: rejected (US-103 AC3).
    REGISTRY_UNAVAILABLE   ///< The backend could not be reached; nothing was written.
};

/**
 * @brief Result of a registry read.
 */
enum class RegistryResult
{
    OK,         ///< The data was read.
    NOT_FOUND,  ///< No node with that node_id.
    UNAVAILABLE ///< The backend could not be reached.
};

/**
 * @brief Result of storing a telemetry sample of an edge device (ADR-028 §1).
 */
enum class TelemetryResult
{
    STORED,       ///< Created or updated with this sample.
    OLDER_SAMPLE, ///< last_seen updated; a newer sample was already shown, so it is kept.
    TCP_NODE,     ///< The node_id belongs to a node registered over TCP: rejected.
    UNAVAILABLE   ///< The backend could not be reached; nothing was written.
};

/** Where a node comes from. */
constexpr const char* NODE_SOURCE_TCP = "tcp";
/** Edge devices, through the ingestor (ADR-028). */
constexpr const char* NODE_SOURCE_EDGE = "edge";

/**
 * @brief One telemetry sample of an edge device, as the `TELEMETRY` command carries it.
 */
struct EdgeTelemetry
{
    std::string nodeId;                               ///< Device node_id.
    std::string bunkerName;                           ///< Device bunker name.
    std::uint32_t messageId = 0;                      ///< Per-boot sequence number.
    std::string bootId;                               ///< 8 hex digits, random per boot.
    double temperature = 0.0;                         ///< Degrees C.
    std::string deviceStatus;                         ///< OK, WARN or ALERT.
    std::int64_t sampledAtMs = 0;                     ///< Device clock at sampling, epoch ms.
    std::chrono::system_clock::time_point receivedAt; ///< Server clock at reception.
};

/**
 * @brief Metadata of one registered bunker node.
 *
 * Populated from a REGISTER message (US-103 AC1) and kept after the client disconnects (AC4).
 */
struct NodeRecord
{
    std::string nodeId;                             ///< Unique identifier sent by the client.
    std::string bunkerName;                         ///< Human-readable bunker name.
    std::string ipAddress;                          ///< IP address reported by the client.
    int capacity;                                   ///< Capacity reported by the client.
    NodeStatus status;                              ///< Effective liveness (ADR-027 §2 for Redis).
    std::chrono::system_clock::time_point lastSeen; ///< Last known activity.
    std::string sessionId;                          ///< Session holding the node, empty if none.
    std::string source = NODE_SOURCE_TCP;           ///< NODE_SOURCE_TCP or NODE_SOURCE_EDGE.
    double temperature = 0.0;                       ///< Edge only: last shown temperature.
    std::string deviceStatus;                       ///< Edge only: OK, WARN or ALERT.
    std::int64_t sampledAtMs = 0;                   ///< Edge only: sampled_at of the shown sample.

    NodeRecord()
        : capacity(0)
        , status(NodeStatus::OFFLINE)
        , lastSeen(std::chrono::system_clock::now())
    {
    }
};

/**
 * @brief Operations of the node registry.
 *
 * @invariant Implementations are safe to call from the server's worker, poll and monitor
 * threads at the same time.
 */
class INodeRegistry
{
public:
    virtual ~INodeRegistry() = default;

    /**
     * @brief Registers a node, or reconnects it if it is OFFLINE with the same metadata.
     * @details Reconnection requires the same bunkerName and capacity; ipAddress is refreshed,
     *          not compared (ADR-018: a reconnecting Pod gets a new IP). The node_id of an edge
     *          node is a duplicate: a node belongs to one source (ADR-028 §1).
     * @param record Candidate record; its status is ignored (a registered node is ONLINE).
     * @return The outcome.
     */
    virtual RegisterNodeResult registerOrReconnectNode(const NodeRecord& record) = 0;

    /**
     * @brief Creates or updates an edge node from a telemetry sample (ADR-028 §1).
     * @details The node gets `source=edge`, `ip_address=0.0.0.0`, `capacity=0` and
     *          `lastSeen=receivedAt`. The shown telemetry is the sample with the newest
     *          `sampledAtMs`, so a buffered sample flushed after a reconnection does not hide a
     *          newer one.
     * @param telemetry The sample.
     * @return The outcome; a node registered over TCP is never overwritten.
     */
    virtual TelemetryResult upsertTelemetry(const EdgeTelemetry& telemetry) = 0;

    /**
     * @brief Looks a node up.
     * @param nodeId node_id to find.
     * @param[out] out Filled when found.
     * @return OK, NOT_FOUND or UNAVAILABLE.
     */
    virtual RegistryResult queryNode(const std::string& nodeId, NodeRecord& out) const = 0;

    /**
     * @brief Lists every registered node (US-103 AC7).
     * @param[out] out Replaced with the nodes.
     * @return OK or UNAVAILABLE.
     */
    virtual RegistryResult listNodes(std::vector<NodeRecord>& out) const = 0;

    /**
     * @brief Binds a registered node to the session that holds it: ONLINE, seen now.
     * @param nodeId Node.
     * @param sessionId Session.
     */
    virtual void bindSession(const std::string& nodeId, const std::string& sessionId) = 0;

    /**
     * @brief Records a heartbeat of a node: ONLINE, seen now.
     * @param nodeId Node.
     */
    virtual void touch(const std::string& nodeId) = 0;

    /**
     * @brief Marks a node OFFLINE because its session ended.
     * @param nodeId Node.
     */
    virtual void markOffline(const std::string& nodeId) = 0;

    /**
     * @brief Whether a node is effectively ONLINE.
     * @param nodeId Node.
     * @return false when unknown or unreachable.
     */
    virtual bool isOnline(const std::string& nodeId) const = 0;

    /**
     * @brief Whether the backend can serve requests (readiness, ADR-027 §3).
     * @return true when usable.
     */
    virtual bool healthy() const = 0;

    /**
     * @brief Removes every node; for tests.
     */
    virtual void clear() = 0;
};

/**
 * @brief Thread-safe registry kept in process memory: the v0.1 behaviour.
 */
class InMemoryNodeRegistry : public INodeRegistry
{
public:
    RegisterNodeResult registerOrReconnectNode(const NodeRecord& record) override;
    TelemetryResult upsertTelemetry(const EdgeTelemetry& telemetry) override;
    RegistryResult queryNode(const std::string& nodeId, NodeRecord& out) const override;
    RegistryResult listNodes(std::vector<NodeRecord>& out) const override;
    void bindSession(const std::string& nodeId, const std::string& sessionId) override;
    void touch(const std::string& nodeId) override;
    void markOffline(const std::string& nodeId) override;
    bool isOnline(const std::string& nodeId) const override;
    bool healthy() const override;
    void clear() override;

private:
    mutable std::mutex m_mutex;                          ///< Guards m_nodes.
    std::unordered_map<std::string, NodeRecord> m_nodes; ///< Nodes by node_id.
};

/**
 * @brief The registry a server replica uses: local nodes in memory, the rest in a shared backend.
 *
 * @details Without a shared backend every node is local, exactly as in v0.1. With one, the nodes
 *          for which `isLocalOnly` is true (the replica's sidecar HPC engine, ADR-027 §3) stay in
 *          this replica's memory and every other node goes to the shared backend; `listNodes`
 *          returns both.
 */
class ReplicaNodeRegistry : public INodeRegistry
{
public:
    /**
     * @brief Creates a registry with every node local.
     * @param isLocalOnly Which node_ids never leave this replica.
     */
    explicit ReplicaNodeRegistry(std::function<bool(const std::string&)> isLocalOnly);

    /**
     * @brief Sends every non-local node to @p shared from now on.
     * @details Safe while other threads use the registry: a call in progress keeps the backend
     *          it started with alive until it returns.
     * @param shared Shared backend; nullptr goes back to all-local.
     */
    void useSharedBackend(std::unique_ptr<INodeRegistry> shared);

    /**
     * @brief Whether a shared backend is in use.
     * @return true with a shared backend.
     */
    bool hasSharedBackend() const;

    RegisterNodeResult registerOrReconnectNode(const NodeRecord& record) override;
    TelemetryResult upsertTelemetry(const EdgeTelemetry& telemetry) override;
    RegistryResult queryNode(const std::string& nodeId, NodeRecord& out) const override;
    RegistryResult listNodes(std::vector<NodeRecord>& out) const override;
    void bindSession(const std::string& nodeId, const std::string& sessionId) override;
    void touch(const std::string& nodeId) override;
    void markOffline(const std::string& nodeId) override;
    bool isOnline(const std::string& nodeId) const override;
    bool healthy() const override;
    void clear() override;

private:
    std::shared_ptr<INodeRegistry> shared() const;
    std::shared_ptr<INodeRegistry> backendFor(const std::string& nodeId) const;

    std::function<bool(const std::string&)> m_isLocalOnly; ///< Routing rule.
    mutable InMemoryNodeRegistry m_local;                  ///< Local nodes (all, without shared).
    mutable std::mutex m_sharedMutex;                      ///< Guards m_shared (not the calls on it).
    std::shared_ptr<INodeRegistry> m_shared;               ///< Shared backend, or nullptr.
};

#endif // NODE_REGISTRY_HPP
