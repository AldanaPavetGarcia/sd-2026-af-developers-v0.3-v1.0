/**
 * @file testMetrics.cpp
 * @brief Unit tests for the Prometheus metrics instrumentation (US-205 / ADR-012).
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <prometheus/client_metric.h>
#include <prometheus/metric_family.h>

#include "../../include/eopServer.hpp"

namespace
{

    /// Port used by this file's server-backed suite (different from the ports already used by
    /// testConnection.cpp/testLoad.cpp/testClient.cpp, to avoid collisions within the same
    /// `unit_tests` binary run).
    const int kMetricsTestPort = 20100 + (getpid() % 2000);
    constexpr int kIdleTimeout = 5;

    /// One-shot: connect, send msg, receive response, close. Returns the response.
    std::string sendAndReceive(const std::string& msg, int port = kMetricsTestPort)
    {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0)
        {
            return "";
        }

        struct sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

        if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0)
        {
            close(sock);
            return "";
        }

        send(sock, msg.c_str(), msg.size(), 0);

        char buf[1024] = {0};
        ssize_t bytes = recv(sock, buf, sizeof(buf) - 1, 0);
        close(sock);

        if (bytes <= 0)
        {
            return "";
        }
        return std::string(buf, static_cast<std::size_t>(bytes));
    }

    /// Finds the MetricFamily with the given name in a Collect() result, or nullptr.
    const prometheus::MetricFamily* findFamily(const std::vector<prometheus::MetricFamily>& families,
                                               const std::string& name)
    {
        for (const auto& family : families)
        {
            if (family.name == name)
            {
                return &family;
            }
        }
        return nullptr;
    }

    /// Finds the sample within a MetricFamily whose label set matches exactly, or nullptr.
    const prometheus::ClientMetric* findSample(const prometheus::MetricFamily& family,
                                               const std::map<std::string, std::string>& labels)
    {
        for (const auto& metric : family.metric)
        {
            std::map<std::string, std::string> metricLabels;
            for (const auto& labelPair : metric.label)
            {
                metricLabels[labelPair.name] = labelPair.value;
            }
            if (metricLabels == labels)
            {
                return &metric;
            }
        }
        return nullptr;
    }

    /**
     * @brief Fixture for tests that call RecordRequestMetric() directly, without a live server.
     *
     * InitMetrics()'s registry/family setup runs unconditionally (even under TEST_MODE, see
     * eopServer.cpp) — only its HTTP exposer bind is skipped — so calling it here is enough to
     * get a fresh, isolated registry per test, with no socket involved.
     */
    class MetricsDirectTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            InitMetrics();
        }
        void TearDown() override
        {
            ShutdownMetrics();
        }
    };

    /**
     * @brief Fixture that spins up a real eop-server instance, for tests that need to drive
     * traffic through the actual TCP protocol / workerLoop() dispatch path.
     */
    class MetricsServerTest : public ::testing::Test
    {
    protected:
        static void SetUpTestSuite()
        {
            // isTerminationSignalReceived is a process-wide global checked by startServer()'s
            // accept loop — every fixture in this binary that starts its own server must reset
            // it here, otherwise a previous suite's TearDownTestSuite() (which sets it true)
            // makes this server exit immediately (same convention as testConnection.cpp /
            // testLoad.cpp).
            isTerminationSignalReceived = false;
            serverThread = std::thread([]() { startServer(kMetricsTestPort, kIdleTimeout); });
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }

        static void TearDownTestSuite()
        {
            isTerminationSignalReceived = true;
            sendAndReceive("WAKEUP", kMetricsTestPort);
            if (serverThread.joinable())
            {
                serverThread.join();
            }
        }

        static std::thread serverThread;
    };

    std::thread MetricsServerTest::serverThread;

} // namespace

// ---------------------------------------------------------------------------
// RecordRequestMetric() — direct calls
// ---------------------------------------------------------------------------

TEST_F(MetricsDirectTest, CounterIncrementsPerLabelSet)
{
    RecordRequestMetric("REGISTER", "OK", 0.01);
    RecordRequestMetric("REGISTER", "OK", 0.02);

    auto families = metricsRegistry.registry->Collect();
    const auto* family = findFamily(families, "eop_requests_total");
    ASSERT_NE(family, nullptr);

    const auto* sample = findSample(*family, {{"command", "REGISTER"}, {"result", "OK"}, {"service", "eop-server"}, {"status", "OK"}});
    ASSERT_NE(sample, nullptr);
    EXPECT_DOUBLE_EQ(sample->counter.value, 2.0);
}

TEST_F(MetricsDirectTest, DifferentLabelsAreCountedSeparately)
{
    RecordRequestMetric("REGISTER", "OK", 0.01);
    RecordRequestMetric("REGISTER", "ERROR", 0.01);
    RecordRequestMetric("HEARTBEAT", "OK", 0.001);

    auto families = metricsRegistry.registry->Collect();
    const auto* family = findFamily(families, "eop_requests_total");
    ASSERT_NE(family, nullptr);

    const auto* registerOk =
        findSample(*family, {{"command", "REGISTER"}, {"result", "OK"}, {"service", "eop-server"}, {"status", "OK"}});
    const auto* registerErr =
        findSample(*family, {{"command", "REGISTER"}, {"result", "ERROR"}, {"service", "eop-server"}, {"status", "ERROR"}});
    const auto* heartbeatOk =
        findSample(*family, {{"command", "HEARTBEAT"}, {"result", "OK"}, {"service", "eop-server"}, {"status", "OK"}});

    ASSERT_NE(registerOk, nullptr);
    ASSERT_NE(registerErr, nullptr);
    ASSERT_NE(heartbeatOk, nullptr);
    EXPECT_DOUBLE_EQ(registerOk->counter.value, 1.0);
    EXPECT_DOUBLE_EQ(registerErr->counter.value, 1.0);
    EXPECT_DOUBLE_EQ(heartbeatOk->counter.value, 1.0);
}

TEST_F(MetricsDirectTest, HistogramObservesDurationAndCount)
{
    RecordRequestMetric("ANALYZE_GRAPH", "OK", 0.05);
    RecordRequestMetric("ANALYZE_GRAPH", "OK", 0.15);

    auto families = metricsRegistry.registry->Collect();
    const auto* family = findFamily(families, "eop_request_duration_seconds");
    ASSERT_NE(family, nullptr);

    const auto* sample = findSample(*family, {{"command", "ANALYZE_GRAPH"}, {"service", "eop-server"}});
    ASSERT_NE(sample, nullptr);
    EXPECT_EQ(sample->histogram.sample_count, 2u);
    EXPECT_DOUBLE_EQ(sample->histogram.sample_sum, 0.20);
}

TEST_F(MetricsDirectTest, ServiceLabelIsConstantAcrossBothFamilies)
{
    RecordRequestMetric("QUERY_NODE", "OK", 0.01);

    auto families = metricsRegistry.registry->Collect();
    const auto* counterFamily = findFamily(families, "eop_requests_total");
    const auto* histogramFamily = findFamily(families, "eop_request_duration_seconds");
    ASSERT_NE(counterFamily, nullptr);
    ASSERT_NE(histogramFamily, nullptr);

    EXPECT_NE(findSample(*counterFamily, {{"command", "QUERY_NODE"}, {"result", "OK"}, {"service", "eop-server"}, {"status", "OK"}}),
              nullptr);
    EXPECT_NE(findSample(*histogramFamily, {{"command", "QUERY_NODE"}, {"service", "eop-server"}}), nullptr);
}

TEST(MetricsUninitializedTest, RecordRequestMetricIsNoOpWithoutInitMetrics)
{
    // No InitMetrics() call in this test — ::metricsRegistry may be left populated by a
    // previous test/suite or freshly default-constructed; force the uninitialized state
    // explicitly so this test doesn't depend on execution order.
    ShutdownMetrics();
    EXPECT_NO_THROW(RecordRequestMetric("HEARTBEAT", "OK", 0.001));
}

// ---------------------------------------------------------------------------
// Regression test: g_logContext.result must not leak across worker loop iterations
// ---------------------------------------------------------------------------

// Before the fix, g_logContext.result/.nodeId/.event were only reset inside workerLoop()'s
// `if (shouldTrace)` branch. Since g_logContext is thread_local and workerLoop() reuses the
// same pool of worker threads across many requests, an untraced HEARTBEAT dispatched on a
// worker thread that had just handled a *traced* ERROR result (e.g. an invalid REGISTER) would
// read that stale "ERROR" value and get counted as HEARTBEAT{status="ERROR"} — even though
// handleHeartbeat() never sets g_logContext.result itself. This drives enough interleaved
// error/heartbeat pairs across independent sockets to exercise every worker thread in the pool
// at least once, and asserts no HEARTBEAT sample was ever recorded with status="ERROR".
TEST_F(MetricsServerTest, HeartbeatNeverInheritsStaleErrorStatus)
{
    constexpr int kPairs = 40; // comfortably above any realistic worker-pool size

    for (int i = 0; i < kPairs; ++i)
    {
        // Guaranteed ERROR: REGISTER without the required bunker_name/ip_address/capacity
        // fields (same pattern as testConnection.cpp's MissingFieldsReturnsError test).
        sendAndReceive("REGISTER node_id=metrics-missing-" + std::to_string(i));
        sendAndReceive(getCmdStringFromCode(CMD_HEARTBEAT));
    }

    auto families = metricsRegistry.registry->Collect();
    const auto* family = findFamily(families, "eop_requests_total");
    ASSERT_NE(family, nullptr);

    const auto* badSample =
        findSample(*family, {{"command", "HEARTBEAT"}, {"result", "ERROR"}, {"service", "eop-server"}, {"status", "ERROR"}});
    EXPECT_EQ(badSample, nullptr) << "HEARTBEAT must never be recorded with status=ERROR "
                                     "(g_logContext.result leaked from a previous request on this worker thread)";

    const auto* goodSample =
        findSample(*family, {{"command", "HEARTBEAT"}, {"result", "OK"}, {"service", "eop-server"}, {"status", "OK"}});
    ASSERT_NE(goodSample, nullptr);
    EXPECT_GE(goodSample->counter.value, static_cast<double>(kPairs));
}
