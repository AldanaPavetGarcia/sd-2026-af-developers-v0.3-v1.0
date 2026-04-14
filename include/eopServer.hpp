#ifndef EOP_SERVER_HPP
#define EOP_SERVER_HPP

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
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
 * @brief Metadata for a single registered bunker node.
 *
 * Populated from a REGISTER message (AC1).
 * Persisted in memory for the lifetime of the server process (AC4).
 */
struct NodeRecord
{
    std::string nodeId;     ///< Unique string identifier sent by the client.
    std::string bunkerName; ///< Human-readable bunker name.
    std::string ipAddress;  ///< IP address reported by the client.
    int capacity;           ///< Capacity value reported by the client.
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
     * @brief Look up a node by its node_id.
     * @param nodeId      The unique id to search for.
     * @param[out] out    Filled with the record when found.
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

// ---------------------------------------------------------------------------
// Server-wide context
// ---------------------------------------------------------------------------

/**
 * @brief Context for managing the server's TCP session state.
 */
struct ServerContext
{
    std::atomic<int> activeConnections;
    std::mutex sessionsMutex;
    NodeRegistry registry; ///< In-memory node registry (AC4).

    ServerContext()
        : activeConnections(0)
    {
    }

    ~ServerContext() {}
};

extern ServerContext serverContext;

// ---------------------------------------------------------------------------
// Server function prototypes
// ---------------------------------------------------------------------------
void handleClient(int clientSock, const std::string& clientIp, int idleTimeoutS);
void startServer(int port, int idleTimeout);
std::string getCurrentTimestamp();

#endif
