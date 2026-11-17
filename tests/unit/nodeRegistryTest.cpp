#include "../../include/nodeRegistry.hpp"

#include <gtest/gtest.h>

namespace
{
    NodeRecord node(const std::string& id, const std::string& bunker = "Vault-1", int capacity = 10)
    {
        NodeRecord record;
        record.nodeId = id;
        record.bunkerName = bunker;
        record.ipAddress = "10.0.0.1";
        record.capacity = capacity;
        return record;
    }

    bool isEngine(const std::string& id)
    {
        return id.rfind("hpc-engine", 0) == 0;
    }

    // A shared backend that is down: every call fails as a dead Redis would.
    class UnreachableRegistry : public INodeRegistry
    {
    public:
        RegisterNodeResult registerOrReconnectNode(const NodeRecord&) override
        {
            return RegisterNodeResult::REGISTRY_UNAVAILABLE;
        }
        TelemetryResult upsertTelemetry(const EdgeTelemetry&) override
        {
            return TelemetryResult::UNAVAILABLE;
        }
        RegistryResult queryNode(const std::string&, NodeRecord&) const override
        {
            return RegistryResult::UNAVAILABLE;
        }
        RegistryResult listNodes(std::vector<NodeRecord>& out) const override
        {
            out.clear();
            return RegistryResult::UNAVAILABLE;
        }
        void bindSession(const std::string&, const std::string&) override {}
        void touch(const std::string&) override {}
        void markOffline(const std::string&) override {}
        bool isOnline(const std::string&) const override
        {
            return false;
        }
        bool healthy() const override
        {
            return false;
        }
        void clear() override {}
    };
} // namespace

// ---------------------------------------------------------------------------
// InMemoryNodeRegistry: the v0.1 behaviour (US-103, ADR-018)
// ---------------------------------------------------------------------------

TEST(InMemoryNodeRegistryTest, RegistersAndQueriesANewNode)
{
    InMemoryNodeRegistry registry;
    NodeRecord out;

    EXPECT_EQ(registry.registerOrReconnectNode(node("n1")), RegisterNodeResult::NEW_NODE);
    ASSERT_EQ(registry.queryNode("n1", out), RegistryResult::OK);
    EXPECT_EQ(out.bunkerName, "Vault-1");
    EXPECT_EQ(out.status, NodeStatus::ONLINE);
    EXPECT_EQ(registry.queryNode("missing", out), RegistryResult::NOT_FOUND);
    EXPECT_TRUE(registry.healthy());
}

TEST(InMemoryNodeRegistryTest, RejectsAnOnlineDuplicate)
{
    InMemoryNodeRegistry registry;
    NodeRecord out;

    registry.registerOrReconnectNode(node("n1"));
    EXPECT_EQ(registry.registerOrReconnectNode(node("n1", "Other", 99)), RegisterNodeResult::DUPLICATE_ONLINE_NODE);
    ASSERT_EQ(registry.queryNode("n1", out), RegistryResult::OK);
    EXPECT_EQ(out.capacity, 10) << "the original entry is kept";
}

TEST(InMemoryNodeRegistryTest, ReconnectsAnOfflineNodeWithTheSameMetadata)
{
    InMemoryNodeRegistry registry;
    NodeRecord out;

    registry.registerOrReconnectNode(node("n1"));
    registry.bindSession("n1", "s-1");
    registry.markOffline("n1");
    ASSERT_EQ(registry.queryNode("n1", out), RegistryResult::OK);
    EXPECT_EQ(out.status, NodeStatus::OFFLINE);
    EXPECT_TRUE(out.sessionId.empty());

    NodeRecord moved = node("n1");
    moved.ipAddress = "10.0.0.99";
    EXPECT_EQ(registry.registerOrReconnectNode(moved), RegisterNodeResult::RECONNECTED_NODE);
    ASSERT_EQ(registry.queryNode("n1", out), RegistryResult::OK);
    EXPECT_EQ(out.ipAddress, "10.0.0.99") << "ADR-018: the IP is refreshed, not compared";
    EXPECT_TRUE(registry.isOnline("n1"));
}

TEST(InMemoryNodeRegistryTest, AnOfflineNodeWithOtherMetadataIsADuplicate)
{
    InMemoryNodeRegistry registry;

    registry.registerOrReconnectNode(node("n1"));
    registry.markOffline("n1");
    EXPECT_EQ(registry.registerOrReconnectNode(node("n1", "Other")), RegisterNodeResult::DUPLICATE_ONLINE_NODE);
}

TEST(InMemoryNodeRegistryTest, SessionAndHeartbeatKeepTheNodeOnline)
{
    InMemoryNodeRegistry registry;
    NodeRecord out;

    registry.registerOrReconnectNode(node("n1"));
    registry.bindSession("n1", "s-7");
    registry.markOffline("n1");
    registry.touch("n1");
    ASSERT_EQ(registry.queryNode("n1", out), RegistryResult::OK);
    EXPECT_EQ(out.status, NodeStatus::ONLINE) << "a heartbeat brings the node back";

    // Unknown nodes are ignored, not created.
    registry.bindSession("ghost", "s-8");
    registry.touch("ghost");
    registry.markOffline("ghost");
    EXPECT_FALSE(registry.isOnline("ghost"));
    EXPECT_EQ(registry.queryNode("ghost", out), RegistryResult::NOT_FOUND);
}

TEST(InMemoryNodeRegistryTest, ListsEveryNodeAndClears)
{
    InMemoryNodeRegistry registry;
    std::vector<NodeRecord> nodes;

    registry.registerOrReconnectNode(node("n1"));
    registry.registerOrReconnectNode(node("n2"));
    ASSERT_EQ(registry.listNodes(nodes), RegistryResult::OK);
    EXPECT_EQ(nodes.size(), 2u);

    registry.clear();
    ASSERT_EQ(registry.listNodes(nodes), RegistryResult::OK);
    EXPECT_TRUE(nodes.empty());
}

// ---------------------------------------------------------------------------
// ReplicaNodeRegistry: engines local, the rest shared (ADR-027 §3)
// ---------------------------------------------------------------------------

TEST(ReplicaNodeRegistryTest, WithoutASharedBackendEverythingIsLocal)
{
    ReplicaNodeRegistry registry(isEngine);
    std::vector<NodeRecord> nodes;

    EXPECT_FALSE(registry.hasSharedBackend());
    registry.registerOrReconnectNode(node("n1"));
    registry.registerOrReconnectNode(node("hpc-engine-a"));
    ASSERT_EQ(registry.listNodes(nodes), RegistryResult::OK);
    EXPECT_EQ(nodes.size(), 2u);
    EXPECT_TRUE(registry.healthy());
}

TEST(ReplicaNodeRegistryTest, EnginesStayLocalAndTheRestGoesToTheSharedBackend)
{
    ReplicaNodeRegistry registry(isEngine);
    auto shared = std::make_unique<InMemoryNodeRegistry>();
    InMemoryNodeRegistry* sharedView = shared.get();
    registry.useSharedBackend(std::move(shared));
    NodeRecord out;
    std::vector<NodeRecord> nodes;

    ASSERT_TRUE(registry.hasSharedBackend());
    registry.registerOrReconnectNode(node("n1"));
    registry.registerOrReconnectNode(node("hpc-engine-a", "HPC", 1));

    EXPECT_EQ(sharedView->queryNode("n1", out), RegistryResult::OK);
    EXPECT_EQ(sharedView->queryNode("hpc-engine-a", out), RegistryResult::NOT_FOUND)
        << "engines never leave the replica";
    EXPECT_EQ(registry.queryNode("hpc-engine-a", out), RegistryResult::OK);

    ASSERT_EQ(registry.listNodes(nodes), RegistryResult::OK);
    EXPECT_EQ(nodes.size(), 2u) << "the list joins both";

    registry.bindSession("n1", "s-1");
    registry.touch("n1");
    EXPECT_TRUE(registry.isOnline("n1"));
    registry.markOffline("n1");
    EXPECT_FALSE(sharedView->isOnline("n1"));
    EXPECT_FALSE(registry.isOnline("n1"));

    registry.clear();
    ASSERT_EQ(registry.listNodes(nodes), RegistryResult::OK);
    EXPECT_TRUE(nodes.empty());
}

TEST(ReplicaNodeRegistryTest, AnUnreachableSharedBackendIsReportedNotHidden)
{
    ReplicaNodeRegistry registry(isEngine);
    registry.useSharedBackend(std::make_unique<UnreachableRegistry>());
    NodeRecord out;
    std::vector<NodeRecord> nodes;

    EXPECT_FALSE(registry.healthy()) << "the replica must not be Ready";
    EXPECT_EQ(registry.registerOrReconnectNode(node("n1")), RegisterNodeResult::REGISTRY_UNAVAILABLE);
    EXPECT_EQ(registry.queryNode("n1", out), RegistryResult::UNAVAILABLE);
    EXPECT_EQ(registry.listNodes(nodes), RegistryResult::UNAVAILABLE);
    EXPECT_TRUE(nodes.empty()) << "no partial list";

    // The local engine keeps working: ANALYZE_GRAPH does not need Redis.
    EXPECT_EQ(registry.registerOrReconnectNode(node("hpc-engine-a", "HPC", 1)), RegisterNodeResult::NEW_NODE);
    EXPECT_TRUE(registry.isOnline("hpc-engine-a"));

    registry.useSharedBackend(nullptr);
    EXPECT_TRUE(registry.healthy());
}

// ---------------------------------------------------------------------------
// Edge devices (ADR-028 §1)
// ---------------------------------------------------------------------------

namespace
{
    EdgeTelemetry sample(const std::string& id, std::int64_t sampledAtMs, double temperature = 21.4)
    {
        EdgeTelemetry t;
        t.nodeId = id;
        t.bunkerName = "Vault-101";
        t.messageId = 1;
        t.bootId = "9f3c27a1";
        t.temperature = temperature;
        t.deviceStatus = "OK";
        t.sampledAtMs = sampledAtMs;
        t.receivedAt = std::chrono::system_clock::now();
        return t;
    }
} // namespace

TEST(InMemoryNodeRegistryTest, TelemetryCreatesAnEdgeNode)
{
    InMemoryNodeRegistry registry;
    NodeRecord out;

    EXPECT_EQ(registry.upsertTelemetry(sample("vault-101", 1000)), TelemetryResult::STORED);
    ASSERT_EQ(registry.queryNode("vault-101", out), RegistryResult::OK);
    EXPECT_EQ(out.source, NODE_SOURCE_EDGE);
    EXPECT_EQ(out.ipAddress, "0.0.0.0");
    EXPECT_EQ(out.capacity, 0);
    EXPECT_DOUBLE_EQ(out.temperature, 21.4);
    EXPECT_EQ(out.deviceStatus, "OK");
    EXPECT_EQ(out.sampledAtMs, 1000);
}

// A buffered sample flushed after a reconnection (US-402) must not hide a newer one.
TEST(InMemoryNodeRegistryTest, AnOlderSampleOnlyRefreshesLastSeen)
{
    InMemoryNodeRegistry registry;
    NodeRecord out;

    registry.upsertTelemetry(sample("vault-101", 2000, 30.0));
    EdgeTelemetry older = sample("vault-101", 1000, 10.0);
    older.receivedAt += std::chrono::seconds(5);
    EXPECT_EQ(registry.upsertTelemetry(older), TelemetryResult::OLDER_SAMPLE);
    ASSERT_EQ(registry.queryNode("vault-101", out), RegistryResult::OK);
    EXPECT_DOUBLE_EQ(out.temperature, 30.0);
    EXPECT_EQ(out.lastSeen, older.receivedAt);

    EXPECT_EQ(registry.upsertTelemetry(sample("vault-101", 3000, 31.5)), TelemetryResult::STORED);
    ASSERT_EQ(registry.queryNode("vault-101", out), RegistryResult::OK);
    EXPECT_DOUBLE_EQ(out.temperature, 31.5);
}

// A node belongs to one source (ADR-028 §1).
TEST(InMemoryNodeRegistryTest, TcpAndEdgeNodeIdsNeverMix)
{
    InMemoryNodeRegistry registry;

    registry.registerOrReconnectNode(node("tcp-1"));
    EXPECT_EQ(registry.upsertTelemetry(sample("tcp-1", 1000)), TelemetryResult::TCP_NODE);

    registry.upsertTelemetry(sample("edge-1", 1000));
    registry.markOffline("edge-1");
    EXPECT_EQ(registry.registerOrReconnectNode(node("edge-1", "Vault-101", 0)),
              RegisterNodeResult::DUPLICATE_ONLINE_NODE);
}

TEST(ReplicaNodeRegistryTest, TelemetryGoesToTheSharedBackend)
{
    ReplicaNodeRegistry registry(isEngine);
    auto shared = std::make_unique<InMemoryNodeRegistry>();
    InMemoryNodeRegistry* sharedView = shared.get();
    registry.useSharedBackend(std::move(shared));
    NodeRecord out;

    EXPECT_EQ(registry.upsertTelemetry(sample("vault-101", 1000)), TelemetryResult::STORED);
    EXPECT_EQ(sharedView->queryNode("vault-101", out), RegistryResult::OK);

    ReplicaNodeRegistry down(isEngine);
    down.useSharedBackend(std::make_unique<UnreachableRegistry>());
    EXPECT_EQ(down.upsertTelemetry(sample("vault-101", 1000)), TelemetryResult::UNAVAILABLE);
}
