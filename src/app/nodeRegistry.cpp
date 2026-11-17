#include "nodeRegistry.hpp"

#include <utility>

namespace
{
    // Edge devices are reached through the broker, never directly (ADR-028 §1).
    constexpr const char* EDGE_IP_ADDRESS = "0.0.0.0";
} // namespace

// ---------------------------------------------------------------------------
// InMemoryNodeRegistry
// ---------------------------------------------------------------------------

RegisterNodeResult InMemoryNodeRegistry::registerOrReconnectNode(const NodeRecord& record)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_nodes.find(record.nodeId);

    if (it == m_nodes.end())
    {
        NodeRecord stored = record;
        stored.status = NodeStatus::ONLINE;
        m_nodes.emplace(record.nodeId, std::move(stored));
        return RegisterNodeResult::NEW_NODE;
    }

    const bool sameMetadata = (it->second.bunkerName == record.bunkerName) && (it->second.capacity == record.capacity);
    // A node belongs to one source: an edge device's node_id cannot register over TCP (ADR-028).
    const bool edgeNode = it->second.source == NODE_SOURCE_EDGE;

    if (it->second.status == NodeStatus::OFFLINE && sameMetadata && !edgeNode)
    {
        it->second.status = NodeStatus::ONLINE;
        it->second.lastSeen = record.lastSeen;
        it->second.ipAddress = record.ipAddress; // ADR-018: updated, not matched
        it->second.sessionId = record.sessionId;
        return RegisterNodeResult::RECONNECTED_NODE;
    }

    return RegisterNodeResult::DUPLICATE_ONLINE_NODE;
}

TelemetryResult InMemoryNodeRegistry::upsertTelemetry(const EdgeTelemetry& telemetry)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_nodes.find(telemetry.nodeId);

    if (it == m_nodes.end())
    {
        NodeRecord record;
        record.nodeId = telemetry.nodeId;
        record.ipAddress = EDGE_IP_ADDRESS;
        record.capacity = 0;
        record.source = NODE_SOURCE_EDGE;
        it = m_nodes.emplace(telemetry.nodeId, std::move(record)).first;
    }
    else if (it->second.source != NODE_SOURCE_EDGE)
    {
        return TelemetryResult::TCP_NODE;
    }

    NodeRecord& node = it->second;
    node.bunkerName = telemetry.bunkerName;
    node.status = NodeStatus::ONLINE;
    node.lastSeen = telemetry.receivedAt;
    if (node.sampledAtMs > telemetry.sampledAtMs)
    {
        return TelemetryResult::OLDER_SAMPLE;
    }
    node.temperature = telemetry.temperature;
    node.deviceStatus = telemetry.deviceStatus;
    node.sampledAtMs = telemetry.sampledAtMs;
    return TelemetryResult::STORED;
}

RegistryResult InMemoryNodeRegistry::queryNode(const std::string& nodeId, NodeRecord& out) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_nodes.find(nodeId);
    if (it == m_nodes.end())
    {
        return RegistryResult::NOT_FOUND;
    }
    out = it->second;
    return RegistryResult::OK;
}

RegistryResult InMemoryNodeRegistry::listNodes(std::vector<NodeRecord>& out) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    out.clear();
    out.reserve(m_nodes.size());
    for (const auto& entry : m_nodes)
    {
        out.push_back(entry.second);
    }
    return RegistryResult::OK;
}

void InMemoryNodeRegistry::bindSession(const std::string& nodeId, const std::string& sessionId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_nodes.find(nodeId);
    if (it != m_nodes.end())
    {
        it->second.status = NodeStatus::ONLINE;
        it->second.lastSeen = std::chrono::system_clock::now();
        it->second.sessionId = sessionId;
    }
}

void InMemoryNodeRegistry::touch(const std::string& nodeId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_nodes.find(nodeId);
    if (it != m_nodes.end())
    {
        it->second.status = NodeStatus::ONLINE;
        it->second.lastSeen = std::chrono::system_clock::now();
    }
}

void InMemoryNodeRegistry::markOffline(const std::string& nodeId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_nodes.find(nodeId);
    if (it != m_nodes.end())
    {
        it->second.status = NodeStatus::OFFLINE;
        it->second.lastSeen = std::chrono::system_clock::now();
        it->second.sessionId.clear();
    }
}

bool InMemoryNodeRegistry::isOnline(const std::string& nodeId) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_nodes.find(nodeId);
    return it != m_nodes.end() && it->second.status == NodeStatus::ONLINE;
}

bool InMemoryNodeRegistry::healthy() const
{
    return true;
}

void InMemoryNodeRegistry::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_nodes.clear();
}

// ---------------------------------------------------------------------------
// ReplicaNodeRegistry
// ---------------------------------------------------------------------------

ReplicaNodeRegistry::ReplicaNodeRegistry(std::function<bool(const std::string&)> isLocalOnly)
    : m_isLocalOnly(std::move(isLocalOnly))
{
}

void ReplicaNodeRegistry::useSharedBackend(std::unique_ptr<INodeRegistry> shared)
{
    std::shared_ptr<INodeRegistry> next = std::move(shared);
    std::lock_guard<std::mutex> lock(m_sharedMutex);
    // The previous backend is released when the last call using it returns.
    m_shared.swap(next);
}

bool ReplicaNodeRegistry::hasSharedBackend() const
{
    return shared() != nullptr;
}

std::shared_ptr<INodeRegistry> ReplicaNodeRegistry::shared() const
{
    std::lock_guard<std::mutex> lock(m_sharedMutex);
    return m_shared;
}

std::shared_ptr<INodeRegistry> ReplicaNodeRegistry::backendFor(const std::string& nodeId) const
{
    std::shared_ptr<INodeRegistry> backend = shared();
    if (backend && !m_isLocalOnly(nodeId))
    {
        return backend;
    }
    // Non-owning: m_local lives as long as this object.
    return std::shared_ptr<INodeRegistry>(std::shared_ptr<INodeRegistry>(), &m_local);
}

RegisterNodeResult ReplicaNodeRegistry::registerOrReconnectNode(const NodeRecord& record)
{
    return backendFor(record.nodeId)->registerOrReconnectNode(record);
}

TelemetryResult ReplicaNodeRegistry::upsertTelemetry(const EdgeTelemetry& telemetry)
{
    return backendFor(telemetry.nodeId)->upsertTelemetry(telemetry);
}

RegistryResult ReplicaNodeRegistry::queryNode(const std::string& nodeId, NodeRecord& out) const
{
    return backendFor(nodeId)->queryNode(nodeId, out);
}

RegistryResult ReplicaNodeRegistry::listNodes(std::vector<NodeRecord>& out) const
{
    const RegistryResult localResult = m_local.listNodes(out);
    const std::shared_ptr<INodeRegistry> backend = shared();
    if (!backend)
    {
        return localResult;
    }

    std::vector<NodeRecord> sharedNodes;
    const RegistryResult sharedResult = backend->listNodes(sharedNodes);
    if (sharedResult != RegistryResult::OK)
    {
        out.clear();
        return sharedResult;
    }
    out.insert(out.end(), sharedNodes.begin(), sharedNodes.end());
    return RegistryResult::OK;
}

void ReplicaNodeRegistry::bindSession(const std::string& nodeId, const std::string& sessionId)
{
    backendFor(nodeId)->bindSession(nodeId, sessionId);
}

void ReplicaNodeRegistry::touch(const std::string& nodeId)
{
    backendFor(nodeId)->touch(nodeId);
}

void ReplicaNodeRegistry::markOffline(const std::string& nodeId)
{
    backendFor(nodeId)->markOffline(nodeId);
}

bool ReplicaNodeRegistry::isOnline(const std::string& nodeId) const
{
    return backendFor(nodeId)->isOnline(nodeId);
}

bool ReplicaNodeRegistry::healthy() const
{
    const std::shared_ptr<INodeRegistry> backend = shared();
    return !backend || backend->healthy();
}

void ReplicaNodeRegistry::clear()
{
    m_local.clear();
    if (const std::shared_ptr<INodeRegistry> backend = shared())
    {
        backend->clear();
    }
}
