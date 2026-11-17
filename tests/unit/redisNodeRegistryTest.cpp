#include "../../include/redisNodeRegistry.hpp"

#include <cstdlib>
#include <gtest/gtest.h>
#include <thread>
#include <unistd.h>

// These tests need a Redis server: EOP_TEST_REDIS_HOST / EOP_TEST_REDIS_PORT (default
// 127.0.0.1:6379; CI runs one as a service). Without one they are skipped. Each test uses its
// own key prefix, so they never see each other's data.

namespace
{
    constexpr int SHORT_TTL_MS = 300;
    constexpr int SHORT_REFRESH_MS = 100;
    constexpr int CLOSED_PORT = 1;

    std::string envOr(const char* name, const char* fallback)
    {
        const char* value = std::getenv(name);
        return (value != nullptr && *value != '\0') ? value : fallback;
    }

    RedisRegistryConfig configFor(const std::string& replica)
    {
        RedisRegistryConfig config;
        config.host = envOr("EOP_TEST_REDIS_HOST", "127.0.0.1");
        config.port = std::atoi(envOr("EOP_TEST_REDIS_PORT", "6379").c_str());
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        config.keyPrefix = std::string("eop-test:") + info->name() + ":" + std::to_string(getpid());
        config.replicaName = replica;
        config.leaseTtlMs = SHORT_TTL_MS;
        config.leaseRefreshMs = SHORT_REFRESH_MS;
        return config;
    }

    NodeRecord node(const std::string& id, const std::string& bunker = "Vault-1", int capacity = 10)
    {
        NodeRecord record;
        record.nodeId = id;
        record.bunkerName = bunker;
        record.ipAddress = "10.0.0.1";
        record.capacity = capacity;
        return record;
    }

    class RedisNodeRegistryTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            RedisNodeRegistry probe(configFor("probe"));
            if (!probe.refreshLease())
            {
                GTEST_SKIP() << "no Redis at EOP_TEST_REDIS_HOST/EOP_TEST_REDIS_PORT";
            }
        }

        void TearDown() override
        {
            RedisNodeRegistry cleaner(configFor("cleaner"));
            cleaner.clear();
        }
    };
} // namespace

TEST_F(RedisNodeRegistryTest, RegistersQueriesAndListsWithEffectiveStatus)
{
    RedisNodeRegistry a(configFor("replica-a"));
    ASSERT_TRUE(a.start());
    NodeRecord out;
    std::vector<NodeRecord> nodes;

    EXPECT_EQ(a.registerOrReconnectNode(node("n1")), RegisterNodeResult::NEW_NODE);
    a.bindSession("n1", "s-1");
    ASSERT_EQ(a.queryNode("n1", out), RegistryResult::OK);
    EXPECT_EQ(out.bunkerName, "Vault-1");
    EXPECT_EQ(out.capacity, 10);
    EXPECT_EQ(out.sessionId, "s-1");
    EXPECT_EQ(out.status, NodeStatus::ONLINE);
    EXPECT_EQ(a.queryNode("missing", out), RegistryResult::NOT_FOUND);

    a.registerOrReconnectNode(node("n2"));
    ASSERT_EQ(a.listNodes(nodes), RegistryResult::OK);
    EXPECT_EQ(nodes.size(), 2u);
    EXPECT_TRUE(a.healthy());
}

// US-103 AC3 across replicas (ADR-027 §2).
TEST_F(RedisNodeRegistryTest, ANodeOnlineOnAnotherLiveReplicaIsADuplicate)
{
    RedisNodeRegistry a(configFor("replica-a"));
    RedisNodeRegistry b(configFor("replica-b"));
    ASSERT_TRUE(a.start());
    ASSERT_TRUE(b.start());

    EXPECT_EQ(a.registerOrReconnectNode(node("n1")), RegisterNodeResult::NEW_NODE);
    EXPECT_EQ(b.registerOrReconnectNode(node("n1")), RegisterNodeResult::DUPLICATE_ONLINE_NODE);
    EXPECT_TRUE(b.isOnline("n1")) << "both replicas see the same registry";
}

// A replica that dies stops refreshing its lease: its nodes read OFFLINE everywhere and can be
// taken over (ADR-027 §2).
TEST_F(RedisNodeRegistryTest, NodesOfADeadReplicaGoOfflineAndCanBeTakenOver)
{
    RedisNodeRegistry b(configFor("replica-b"));
    ASSERT_TRUE(b.start());
    {
        // No start(): one lease and no refresh thread, as a replica that crashed right after.
        RedisNodeRegistry a(configFor("replica-a"));
        ASSERT_TRUE(a.refreshLease());
        ASSERT_EQ(a.registerOrReconnectNode(node("n1")), RegisterNodeResult::NEW_NODE);
        EXPECT_TRUE(b.isOnline("n1"));
        std::this_thread::sleep_for(std::chrono::milliseconds(SHORT_TTL_MS * 2));
        EXPECT_FALSE(b.isOnline("n1")) << "lease expired: OFFLINE without anyone writing";
        // The bunker reconnects through the other replica.
        EXPECT_EQ(b.registerOrReconnectNode(node("n1")), RegisterNodeResult::RECONNECTED_NODE);
        EXPECT_TRUE(b.isOnline("n1"));
        // The dead replica can no longer touch a node it lost.
        a.markOffline("n1");
    }
    EXPECT_TRUE(b.isOnline("n1"));
}

TEST_F(RedisNodeRegistryTest, AGracefulShutdownHandsTheNodesOverAtOnce)
{
    RedisNodeRegistry b(configFor("replica-b"));
    ASSERT_TRUE(b.start());
    {
        RedisNodeRegistry a(configFor("replica-a"));
        ASSERT_TRUE(a.start());
        a.registerOrReconnectNode(node("n1"));
        EXPECT_TRUE(b.isOnline("n1"));
    }
    EXPECT_FALSE(b.isOnline("n1")) << "the destructor released the lease";
}

TEST_F(RedisNodeRegistryTest, OfflineNodesReconnectAndHeartbeatsKeepThemOnline)
{
    RedisNodeRegistry a(configFor("replica-a"));
    ASSERT_TRUE(a.start());
    NodeRecord out;

    a.registerOrReconnectNode(node("n1"));
    a.markOffline("n1");
    ASSERT_EQ(a.queryNode("n1", out), RegistryResult::OK);
    EXPECT_EQ(out.status, NodeStatus::OFFLINE);
    EXPECT_TRUE(out.sessionId.empty());
    EXPECT_EQ(a.registerOrReconnectNode(node("n1", "Other")), RegisterNodeResult::DUPLICATE_ONLINE_NODE)
        << "other metadata is not a reconnection";
    EXPECT_EQ(a.registerOrReconnectNode(node("n1")), RegisterNodeResult::RECONNECTED_NODE);

    const auto before = std::chrono::system_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    a.touch("n1");
    ASSERT_EQ(a.queryNode("n1", out), RegistryResult::OK);
    EXPECT_GT(out.lastSeen, before);
    EXPECT_EQ(out.status, NodeStatus::ONLINE);
}

// Redis keeps no persistence (ADR-010 §5): after it loses the data, the replicas write their live
// nodes again when they reconnect.
TEST_F(RedisNodeRegistryTest, OwnedNodesAreRewrittenAfterRedisLosesThem)
{
    RedisNodeRegistry a(configFor("replica-a"));
    ASSERT_TRUE(a.refreshLease());
    a.registerOrReconnectNode(node("n1"));
    a.bindSession("n1", "s-1");

    RedisNodeRegistry wiper(configFor("wiper"));
    wiper.clear();
    NodeRecord out;
    ASSERT_EQ(a.queryNode("n1", out), RegistryResult::NOT_FOUND);

    a.dropConnectionForTest();
    ASSERT_TRUE(a.refreshLease());
    ASSERT_EQ(a.queryNode("n1", out), RegistryResult::OK);
    EXPECT_EQ(out.sessionId, "s-1");
    EXPECT_EQ(out.status, NodeStatus::ONLINE);
}

TEST(RedisNodeRegistryDownTest, AnUnreachableRedisIsReportedNotHidden)
{
    RedisRegistryConfig config;
    config.host = "127.0.0.1";
    config.port = CLOSED_PORT;
    config.replicaName = "replica-a";
    RedisNodeRegistry registry(config);
    NodeRecord out;
    std::vector<NodeRecord> nodes;

    EXPECT_FALSE(registry.start());
    EXPECT_FALSE(registry.healthy());
    EXPECT_EQ(registry.registerOrReconnectNode(node("n1")), RegisterNodeResult::REGISTRY_UNAVAILABLE);
    EXPECT_EQ(registry.queryNode("n1", out), RegistryResult::UNAVAILABLE);
    EXPECT_EQ(registry.listNodes(nodes), RegistryResult::UNAVAILABLE);
    EXPECT_FALSE(registry.isOnline("n1"));
    registry.touch("n1");
    registry.bindSession("n1", "s");
    registry.markOffline("n1");
    EXPECT_FALSE(registry.healthy());
}

// ---------------------------------------------------------------------------
// Configuration from the environment
// ---------------------------------------------------------------------------

namespace
{
    class RedisConfigEnvTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            clear();
        }
        void TearDown() override
        {
            clear();
        }

        static void clear()
        {
            for (const char* name : {"EOP_REDIS_HOST",
                                     "EOP_REDIS_PORT",
                                     "EOP_REGISTRY_PREFIX",
                                     "EOP_REPLICA_NAME",
                                     "EOP_REGISTRY_LEASE_TTL_MS",
                                     "EOP_REGISTRY_LEASE_REFRESH_MS",
                                     "EOP_REDIS_TIMEOUT_MS",
                                     "HOSTNAME"})
            {
                unsetenv(name);
            }
        }
    };
} // namespace

TEST_F(RedisConfigEnvTest, ReadsEveryVariable)
{
    setenv("EOP_REDIS_HOST", "redis.eop", 1);
    setenv("EOP_REDIS_PORT", "6380", 1);
    setenv("EOP_REGISTRY_PREFIX", "eop-prod", 1);
    setenv("EOP_REPLICA_NAME", "eop-server-abc", 1);
    setenv("EOP_REGISTRY_LEASE_TTL_MS", "4000", 1);
    setenv("EOP_REGISTRY_LEASE_REFRESH_MS", "500", 1);
    setenv("EOP_REDIS_TIMEOUT_MS", "150", 1);
    RedisRegistryConfig config;
    std::string error;

    ASSERT_TRUE(loadRedisRegistryConfig(config, error)) << error;
    EXPECT_EQ(config.host, "redis.eop");
    EXPECT_EQ(config.port, 6380);
    EXPECT_EQ(config.keyPrefix, "eop-prod");
    EXPECT_EQ(config.replicaName, "eop-server-abc");
    EXPECT_EQ(config.leaseTtlMs, 4000);
    EXPECT_EQ(config.leaseRefreshMs, 500);
    EXPECT_EQ(config.commandTimeoutMs, 150);
}

TEST_F(RedisConfigEnvTest, FallsBackToTheHostnameForTheReplicaName)
{
    setenv("HOSTNAME", "eop-server-pod", 1);
    RedisRegistryConfig config;
    std::string error;

    ASSERT_TRUE(loadRedisRegistryConfig(config, error)) << error;
    EXPECT_EQ(config.replicaName, "eop-server-pod");
    EXPECT_EQ(config.host, "redis") << "default host";
}

TEST_F(RedisConfigEnvTest, RejectsBadValues)
{
    RedisRegistryConfig config;
    std::string error;

    EXPECT_FALSE(loadRedisRegistryConfig(config, error)) << "no replica name";
    EXPECT_NE(error.find("EOP_REPLICA_NAME"), std::string::npos);

    setenv("EOP_REPLICA_NAME", "r", 1);
    setenv("EOP_REDIS_PORT", "63x9", 1);
    EXPECT_FALSE(loadRedisRegistryConfig(config, error));
    EXPECT_NE(error.find("EOP_REDIS_PORT"), std::string::npos);

    unsetenv("EOP_REDIS_PORT");
    setenv("EOP_REGISTRY_LEASE_REFRESH_MS", "5000", 1);
    EXPECT_FALSE(loadRedisRegistryConfig(config, error)) << "refresh must be shorter than the TTL";
}
