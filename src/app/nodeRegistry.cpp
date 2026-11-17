#include "nodeRegistry.hpp"

#include <utility>

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

    if (it->second.status == NodeStatus::OFFLINE && sameMetadata)
    {
        it->second.status = NodeStatus::ONLINE;
        it->second.lastSeen = record.lastSeen;
        it->second.ipAddress = record.ipAddress; // ADR-018: updated, not matched
        it->second.sessionId = record.sessionId;
        return RegisterNodeResult::RECONNECTED_NODE;
    }

    return RegisterNodeResult::DUPLICATE_ONLINE_NODE;
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
    m_shared = std::move(shared);
}

bool ReplicaNodeRegistry::hasSharedBackend() const
{
    return m_shared != nullptr;
}

INodeRegistry& ReplicaNodeRegistry::backendFor(const std::string& nodeId) const
{
    if (m_shared && !m_isLocalOnly(nodeId))
    {
        return *m_shared;
    }
    return m_local;
}

RegisterNodeResult ReplicaNodeRegistry::registerOrReconnectNode(const NodeRecord& record)
{
    return backendFor(record.nodeId).registerOrReconnectNode(record);
}

RegistryResult ReplicaNodeRegistry::queryNode(const std::string& nodeId, NodeRecord& out) const
{
    return backendFor(nodeId).queryNode(nodeId, out);
}

RegistryResult ReplicaNodeRegistry::listNodes(std::vector<NodeRecord>& out) const
{
    const RegistryResult localResult = m_local.listNodes(out);
    if (!m_shared)
    {
        return localResult;
    }

    std::vector<NodeRecord> shared;
    const RegistryResult sharedResult = m_shared->listNodes(shared);
    if (sharedResult != RegistryResult::OK)
    {
        out.clear();
        return sharedResult;
    }
    out.insert(out.end(), shared.begin(), shared.end());
    return RegistryResult::OK;
}

void ReplicaNodeRegistry::bindSession(const std::string& nodeId, const std::string& sessionId)
{
    backendFor(nodeId).bindSession(nodeId, sessionId);
}

void ReplicaNodeRegistry::touch(const std::string& nodeId)
{
    backendFor(nodeId).touch(nodeId);
}

void ReplicaNodeRegistry::markOffline(const std::string& nodeId)
{
    backendFor(nodeId).markOffline(nodeId);
}

bool ReplicaNodeRegistry::isOnline(const std::string& nodeId) const
{
    return backendFor(nodeId).isOnline(nodeId);
}

bool ReplicaNodeRegistry::healthy() const
{
    return !m_shared || m_shared->healthy();
}

void ReplicaNodeRegistry::clear()
{
    m_local.clear();
    if (m_shared)
    {
        m_shared->clear();
    }
}
