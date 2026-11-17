/**
 * @file redisNodeRegistry.hpp
 * @brief Node registry shared by every eop-server replica through Redis (ADR-027).
 *
 * @details Data model (ADR-027 §2), with `<p>` the configurable key prefix:
 *          - `<p>:node:<node_id>`: hash with the v0.1 fields, `source`, `owner` (the replica
 *            holding the node's session) and `last_seen` in epoch milliseconds; edge nodes
 *            (ADR-028) have no owner and add `temperature`, `device_status`, `sampled_at`;
 *          - `<p>:nodes`: set of every node_id, for LIST_NODES;
 *          - `<p>:replica:<name>`: this replica's lease, refreshed every `leaseRefreshMs` with a
 *            `leaseTtlMs` expiry.
 *
 *          A node is effectively ONLINE only while its stored status is ONLINE **and** its
 *          owner's lease exists, so the nodes of a replica that dies read OFFLINE everywhere within
 *          one lease TTL without anyone writing to them. REGISTER is one Lua script, atomic in
 *          Redis, so a node ONLINE on a live replica is rejected as a duplicate across replicas
 *          (US-103 AC3) and one whose owner is gone is taken over.
 *
 *          The replica keeps a copy of the nodes it owns and writes them again when its Redis
 *          connection comes back, so a Redis restart (no persistence, ADR-010 §5) is repaired by
 *          the live sessions. Every Redis call has a timeout; failures are return codes and make
 *          `healthy()` false until the next successful call.
 */

#ifndef REDIS_NODE_REGISTRY_HPP
#define REDIS_NODE_REGISTRY_HPP

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "nodeRegistry.hpp"

struct redisContext;
struct redisReply;

/**
 * @brief Settings of the Redis registry, from the environment (ADR-027, config/ConfigMap).
 */
struct RedisRegistryConfig
{
    std::string host = "redis";    ///< EOP_REDIS_HOST.
    int port = 6379;               ///< EOP_REDIS_PORT.
    std::string keyPrefix = "eop"; ///< EOP_REGISTRY_PREFIX.
    std::string replicaName;       ///< EOP_REPLICA_NAME (Pod name), required.
    int leaseTtlMs = 3000;         ///< EOP_REGISTRY_LEASE_TTL_MS.
    int leaseRefreshMs = 1000;     ///< EOP_REGISTRY_LEASE_REFRESH_MS.
    int commandTimeoutMs = 200;    ///< EOP_REDIS_TIMEOUT_MS: connect and per-command timeout.
};

/**
 * @brief INodeRegistry kept in Redis and shared by the server replicas.
 *
 * @invariant The lease thread runs from start() until the object is destroyed; the destructor
 * stops it and deletes this replica's lease, so a graceful shutdown hands the nodes over at once.
 */
class RedisNodeRegistry : public INodeRegistry
{
public:
    /**
     * @brief Prepares the registry; no connection is made until start().
     * @param config Settings.
     */
    explicit RedisNodeRegistry(RedisRegistryConfig config);

    /**
     * @brief Stops the lease thread and releases this replica's lease.
     */
    ~RedisNodeRegistry() override;

    RedisNodeRegistry(const RedisNodeRegistry&) = delete;
    RedisNodeRegistry& operator=(const RedisNodeRegistry&) = delete;

    /**
     * @brief Connects, takes the lease and starts refreshing it.
     * @return true if Redis answered; the lease thread keeps retrying either way.
     */
    bool start();

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

    /**
     * @brief Refreshes the lease once and, after a reconnection, rewrites the owned nodes.
     * @details Called by the lease thread every `leaseRefreshMs`; public for tests.
     * @return true if Redis answered.
     */
    bool refreshLease();

    /**
     * @brief Drops the current connection, as a network failure would; for tests.
     */
    void dropConnectionForTest();

private:
    /** Frees a hiredis context. */
    struct ContextDeleter
    {
        void operator()(redisContext* ctx) const;
    };
    /** Frees a hiredis reply. */
    struct ReplyDeleter
    {
        void operator()(redisReply* reply) const;
    };
    using ContextPtr = std::unique_ptr<redisContext, ContextDeleter>;
    using ReplyPtr = std::unique_ptr<redisReply, ReplyDeleter>;

    ReplyPtr command(const std::vector<std::string>& args) const;
    bool connectLocked() const;
    bool ownerUpdate(const std::string& nodeId, const std::vector<std::string>& fieldValues);
    bool writeOwnedNode(const NodeRecord& record);
    std::string nodeKey(const std::string& nodeId) const;
    std::string leaseKey(const std::string& replica) const;
    std::string indexKey() const;
    void leaseLoop();

    RedisRegistryConfig m_config;                        ///< Settings.
    mutable std::mutex m_connMutex;                      ///< Guards m_ctx and every command.
    mutable ContextPtr m_ctx;                            ///< Connection, nullptr when down.
    mutable std::atomic<bool> m_healthy {false};         ///< Last command succeeded.
    mutable std::atomic<bool> m_needResync {false};      ///< Rewrite owned nodes on the next lease.
    std::mutex m_ownedMutex;                             ///< Guards m_owned.
    std::unordered_map<std::string, NodeRecord> m_owned; ///< Nodes this replica holds.
    std::mutex m_stopMutex;                              ///< Guards m_stopping for m_stopCv.
    std::condition_variable m_stopCv;                    ///< Wakes the lease thread to stop.
    bool m_stopping = false;                             ///< Set by the destructor.
    std::thread m_leaseThread;                           ///< Refreshes the lease.
};

/**
 * @brief Reads RedisRegistryConfig from the environment (EOP_REDIS_HOST, EOP_REDIS_PORT,
 *        EOP_REGISTRY_PREFIX, EOP_REPLICA_NAME, EOP_REGISTRY_LEASE_TTL_MS,
 *        EOP_REGISTRY_LEASE_REFRESH_MS, EOP_REDIS_TIMEOUT_MS).
 * @param[out] config Filled from the environment and defaults.
 * @param[out] error Why the configuration is invalid.
 * @return false if a value is malformed or EOP_REPLICA_NAME and HOSTNAME are both unset.
 */
bool loadRedisRegistryConfig(RedisRegistryConfig& config, std::string& error);

#endif // REDIS_NODE_REGISTRY_HPP
