#include "hpc/metrics.hpp"

#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <omp.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "hpc/engine.hpp"

#include <prometheus/client_metric.h>
#include <prometheus/metric_family.h>

namespace
{

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

class HpcMetricsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        hpc::InitHpcMetrics(false);
    }

    void TearDown() override
    {
        hpc::ShutdownHpcMetrics();
    }
};

} // namespace

TEST_F(HpcMetricsTest, RecordsAckRequestMetrics)
{
    hpc::HpcRequestMetrics metrics;
    metrics.status = "ACK";
    metrics.processing_seconds = 0.125;
    metrics.thread_count = 4;
    metrics.graph_nodes = 12;
    metrics.graph_edges = 20;
    metrics.algorithm_durations_seconds = {
        {"shortest_paths", 0.050},
        {"connected_components", 0.025},
        {"centrality", 0.075},
    };

    hpc::RecordHpcRequestMetric(metrics);

    const auto families = hpc::hpcMetricsRegistry.registry->Collect();

    const auto* requestsFamily = findFamily(families, "eop_hpc_requests_total");
    ASSERT_NE(requestsFamily, nullptr);
    const auto* requestSample = findSample(*requestsFamily, {{"service", "hpc-engine"}, {"status", "ACK"}});
    ASSERT_NE(requestSample, nullptr);
    EXPECT_DOUBLE_EQ(requestSample->counter.value, 1.0);

    const auto* processingFamily = findFamily(families, "eop_hpc_processing_duration_seconds");
    ASSERT_NE(processingFamily, nullptr);
    const auto* processingSample = findSample(*processingFamily, {{"service", "hpc-engine"}});
    ASSERT_NE(processingSample, nullptr);
    EXPECT_EQ(processingSample->histogram.sample_count, 1u);
    EXPECT_DOUBLE_EQ(processingSample->histogram.sample_sum, 0.125);

    const auto* algorithmFamily = findFamily(families, "eop_hpc_algorithm_duration_seconds");
    ASSERT_NE(algorithmFamily, nullptr);
    const auto* shortestPathsSample =
        findSample(*algorithmFamily, {{"algorithm", "shortest_paths"}, {"service", "hpc-engine"}});
    ASSERT_NE(shortestPathsSample, nullptr);
    EXPECT_EQ(shortestPathsSample->histogram.sample_count, 1u);
    EXPECT_DOUBLE_EQ(shortestPathsSample->histogram.sample_sum, 0.050);

    const auto* threadFamily = findFamily(families, "eop_hpc_thread_count");
    ASSERT_NE(threadFamily, nullptr);
    const auto* threadSample = findSample(*threadFamily, {{"service", "hpc-engine"}});
    ASSERT_NE(threadSample, nullptr);
    EXPECT_DOUBLE_EQ(threadSample->gauge.value, 4.0);

    const auto* nodesFamily = findFamily(families, "eop_hpc_graph_nodes");
    ASSERT_NE(nodesFamily, nullptr);
    const auto* nodesSample = findSample(*nodesFamily, {{"service", "hpc-engine"}});
    ASSERT_NE(nodesSample, nullptr);
    EXPECT_DOUBLE_EQ(nodesSample->gauge.value, 12.0);

    const auto* edgesFamily = findFamily(families, "eop_hpc_graph_edges");
    ASSERT_NE(edgesFamily, nullptr);
    const auto* edgesSample = findSample(*edgesFamily, {{"service", "hpc-engine"}});
    ASSERT_NE(edgesSample, nullptr);
    EXPECT_DOUBLE_EQ(edgesSample->gauge.value, 20.0);
}

TEST_F(HpcMetricsTest, RecordsErrorRequestWithoutProcessingGauges)
{
    hpc::RecordHpcErrorMetric("ERROR");

    const auto families = hpc::hpcMetricsRegistry.registry->Collect();

    const auto* requestsFamily = findFamily(families, "eop_hpc_requests_total");
    ASSERT_NE(requestsFamily, nullptr);
    const auto* errorSample = findSample(*requestsFamily, {{"service", "hpc-engine"}, {"status", "ERROR"}});
    ASSERT_NE(errorSample, nullptr);
    EXPECT_DOUBLE_EQ(errorSample->counter.value, 1.0);
}

TEST_F(HpcMetricsTest, RecordsSpeedupOnlyWhenExplicitlyProvided)
{
    hpc::RecordHpcSpeedupRatio(2.5);

    const auto families = hpc::hpcMetricsRegistry.registry->Collect();

    const auto* speedupFamily = findFamily(families, "eop_hpc_speedup_ratio");
    ASSERT_NE(speedupFamily, nullptr);
    const auto* speedupSample = findSample(*speedupFamily, {{"service", "hpc-engine"}});
    ASSERT_NE(speedupSample, nullptr);
    EXPECT_DOUBLE_EQ(speedupSample->gauge.value, 2.5);
}

TEST(HpcMetricsUninitializedTest, RecordersAreNoOpWithoutInit)
{
    hpc::ShutdownHpcMetrics();

    hpc::HpcRequestMetrics metrics;
    metrics.status = "ACK";
    EXPECT_NO_THROW(hpc::RecordHpcRequestMetric(metrics));
    EXPECT_NO_THROW(hpc::RecordHpcErrorMetric("ERROR"));
    EXPECT_NO_THROW(hpc::RecordHpcSpeedupRatio(1.0));
}

// --- Configured baseline / speedup ratio (US-206 AC1, AC4) ---

TEST(ComputeConfiguredSpeedupRatioTest, NoConfigReturnsFalse)
{
    unsetenv("EOP_HPC_BASELINE_P99_MS");
    unsetenv("EOP_HPC_BASELINE_GRAPH_NODES");

    double speedup = 0.0;
    EXPECT_FALSE(hpc::ComputeConfiguredSpeedupRatio(1000, 0.5, speedup));
}

TEST(ComputeConfiguredSpeedupRatioTest, WithinBandComputesSpeedup)
{
    setenv("EOP_HPC_BASELINE_P99_MS", "2000", 1); // 2s baseline
    setenv("EOP_HPC_BASELINE_GRAPH_NODES", "1000", 1);
    unsetenv("EOP_HPC_BASELINE_BAND_PERCENT"); // default 20%

    double speedup = 0.0;
    // 1050 nodes is within 20% of the 1000-node reference.
    const bool applicable = hpc::ComputeConfiguredSpeedupRatio(1050, 0.5, speedup);

    unsetenv("EOP_HPC_BASELINE_P99_MS");
    unsetenv("EOP_HPC_BASELINE_GRAPH_NODES");

    ASSERT_TRUE(applicable);
    EXPECT_DOUBLE_EQ(speedup, 4.0); // 2.0s baseline / 0.5s actual = 4x
}

TEST(ComputeConfiguredSpeedupRatioTest, OutsideBandReturnsFalse)
{
    setenv("EOP_HPC_BASELINE_P99_MS", "2000", 1);
    setenv("EOP_HPC_BASELINE_GRAPH_NODES", "1000", 1);
    unsetenv("EOP_HPC_BASELINE_BAND_PERCENT"); // default 20%

    double speedup = 0.0;
    // 5000 nodes is nowhere near the 1000-node reference -- comparing a baseline measured at a
    // very different scale would be misleading, so this must not report a speedup.
    const bool applicable = hpc::ComputeConfiguredSpeedupRatio(5000, 0.5, speedup);

    unsetenv("EOP_HPC_BASELINE_P99_MS");
    unsetenv("EOP_HPC_BASELINE_GRAPH_NODES");

    EXPECT_FALSE(applicable);
}

TEST(ComputeConfiguredSpeedupRatioTest, MalformedBaselineFallsBackToNotConfigured)
{
    setenv("EOP_HPC_BASELINE_P99_MS", "not-a-number", 1);
    setenv("EOP_HPC_BASELINE_GRAPH_NODES", "1000", 1);

    double speedup = 0.0;
    const bool applicable = hpc::ComputeConfiguredSpeedupRatio(1000, 0.5, speedup);

    unsetenv("EOP_HPC_BASELINE_P99_MS");
    unsetenv("EOP_HPC_BASELINE_GRAPH_NODES");

    EXPECT_FALSE(applicable);
}

TEST_F(HpcMetricsTest, RecordHpcRequestMetricPublishesSpeedupWhenBaselineConfigured)
{
    setenv("EOP_HPC_BASELINE_P99_MS", "1000", 1); // 1s baseline
    setenv("EOP_HPC_BASELINE_GRAPH_NODES", "500", 1);

    hpc::HpcRequestMetrics metrics;
    metrics.status = "ACK";
    metrics.processing_seconds = 0.25;
    metrics.graph_nodes = 500; // exact match, well within any band
    hpc::RecordHpcRequestMetric(metrics);

    unsetenv("EOP_HPC_BASELINE_P99_MS");
    unsetenv("EOP_HPC_BASELINE_GRAPH_NODES");

    const auto families = hpc::hpcMetricsRegistry.registry->Collect();
    const auto* speedupFamily = findFamily(families, "eop_hpc_speedup_ratio");
    ASSERT_NE(speedupFamily, nullptr);
    const auto* speedupSample = findSample(*speedupFamily, {{"service", "hpc-engine"}});
    ASSERT_NE(speedupSample, nullptr);
    EXPECT_DOUBLE_EQ(speedupSample->gauge.value, 4.0); // 1.0s / 0.25s
}

TEST_F(HpcMetricsTest, InitHpcMetricsExposesBaselineSecondsWhenConfigured)
{
    setenv("EOP_HPC_BASELINE_P99_MS", "1500", 1); // 1.5s
    hpc::ShutdownHpcMetrics();
    hpc::InitHpcMetrics(false);
    unsetenv("EOP_HPC_BASELINE_P99_MS");

    const auto families = hpc::hpcMetricsRegistry.registry->Collect();
    const auto* baselineFamily = findFamily(families, "eop_hpc_baseline_seconds");
    ASSERT_NE(baselineFamily, nullptr);
    const auto* baselineSample = findSample(*baselineFamily, {{"service", "hpc-engine"}});
    ASSERT_NE(baselineSample, nullptr);
    EXPECT_DOUBLE_EQ(baselineSample->gauge.value, 1.5);
}


// --- US-206 AC6: active thread utilisation -------------------------------------------------

TEST_F(HpcMetricsTest, ActiveThreadScopeCountsEveryWorkerInTheRegion)
{
    ASSERT_EQ(hpc::CurrentActiveThreadCount(), 0);

    constexpr int REQUESTED_THREADS = 4;
    int teamSize = 0;
    int observedInsideRegion = 0;

#pragma omp parallel num_threads(REQUESTED_THREADS)
    {
        hpc::ActiveThreadScope activeThreadScope;

        // Every worker holds its scope until all of them have entered, so the count read below
        // is the whole team rather than whatever subset happened to overlap.
#pragma omp barrier

#pragma omp single
        {
            teamSize = omp_get_num_threads();
            observedInsideRegion = hpc::CurrentActiveThreadCount();
        }
    }

    EXPECT_EQ(observedInsideRegion, teamSize);
    EXPECT_EQ(hpc::CurrentActiveThreadCount(), 0) << "workers must be released when the region ends";
}

TEST_F(HpcMetricsTest, ActiveThreadScopePublishesGaugeAndBusySeconds)
{
    {
        hpc::ActiveThreadScope activeThreadScope;

        const auto families = hpc::hpcMetricsRegistry.registry->Collect();
        const auto* gauge = findFamily(families, "eop_hpc_active_threads");
        ASSERT_NE(gauge, nullptr);
        const auto* sample = findSample(*gauge, {{"service", "hpc-engine"}});
        ASSERT_NE(sample, nullptr);
        EXPECT_DOUBLE_EQ(sample->gauge.value, 1.0);
    }

    const auto families = hpc::hpcMetricsRegistry.registry->Collect();

    const auto* gauge = findFamily(families, "eop_hpc_active_threads");
    ASSERT_NE(gauge, nullptr);
    const auto* gaugeSample = findSample(*gauge, {{"service", "hpc-engine"}});
    ASSERT_NE(gaugeSample, nullptr);
    EXPECT_DOUBLE_EQ(gaugeSample->gauge.value, 0.0) << "gauge must fall back to idle after the scope";

    const auto* busy = findFamily(families, "eop_hpc_thread_busy_seconds_total");
    ASSERT_NE(busy, nullptr);
    const auto* busySample = findSample(*busy, {{"service", "hpc-engine"}});
    ASSERT_NE(busySample, nullptr);
    EXPECT_GT(busySample->counter.value, 0.0) << "the scope's elapsed time must be accumulated";
}

TEST_F(HpcMetricsTest, AnalysisAccumulatesThreadBusySecondsAndLeavesNoWorkersActive)
{
    const std::string request = R"({"type":"ANALYZE_GRAPH","request_id":"ac6",)"
                               R"("payload":{"nodes":["a","b","c","d"],)"
                               R"("edges":[{"from":"a","to":"b","weight":1},)"
                               R"({"from":"b","to":"c","weight":1},)"
                               R"({"from":"c","to":"d","weight":1}],)"
                               R"("algorithms":["connected_components","centrality"]}})";

    const auto result = hpc::processAnalyzeGraphRequest(request);
    ASSERT_EQ(result.exit_code, 0) << result.payload;

    EXPECT_EQ(hpc::CurrentActiveThreadCount(), 0);

    const auto families = hpc::hpcMetricsRegistry.registry->Collect();
    const auto* busy = findFamily(families, "eop_hpc_thread_busy_seconds_total");
    ASSERT_NE(busy, nullptr);
    const auto* busySample = findSample(*busy, {{"service", "hpc-engine"}});
    ASSERT_NE(busySample, nullptr);
    EXPECT_GT(busySample->counter.value, 0.0)
        << "the algorithms' parallel regions must report the time their workers spent busy";
}

TEST(HpcMetricsUninitializedTest, ActiveThreadScopeIsANoOpWithoutInit)
{
    hpc::ShutdownHpcMetrics();

    {
        // Must neither crash nor write through the cleared child pointers.
        hpc::ActiveThreadScope activeThreadScope;
        EXPECT_EQ(hpc::CurrentActiveThreadCount(), 1);
    }

    EXPECT_EQ(hpc::CurrentActiveThreadCount(), 0);
}

// --- /metrics exposer ----------------------------------------------------------------------
// InitHpcMetrics(true) is the path the engine actually takes in socket mode. It was never
// exercised in-process: the smoke test covers it against the real binary, but that binary is
// stopped with a signal, so it contributes no coverage and no assertion about the port
// selection logic.

namespace
{

// Bind port 0, read what the kernel assigned, release it. Racy in principle, but it is what
// the Python smoke tests do and it keeps the test from colliding with a fixed port.
int reserveFreePort()
{
    const int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_GE(sock, 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    EXPECT_EQ(::bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

    socklen_t length = sizeof(addr);
    EXPECT_EQ(::getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &length), 0);

    const int port = ntohs(addr.sin_port);
    ::close(sock);

    return port;
}

std::string httpGetMetrics(int port)
{
    const int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_GE(sock, 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));

    timeval timeout{};
    timeout.tv_sec = 5;
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        ::close(sock);
        return "";
    }

    const std::string request = "GET /metrics HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    EXPECT_GT(::send(sock, request.data(), request.size(), 0), 0);

    std::string response;
    char buffer[4096];
    ssize_t received = 0;
    while ((received = ::recv(sock, buffer, sizeof(buffer), 0)) > 0)
    {
        response.append(buffer, static_cast<std::size_t>(received));
    }

    ::close(sock);

    return response;
}

} // namespace

TEST(HpcMetricsExposerTest, ServesTheRegistryOverHttpOnTheConfiguredPort)
{
    hpc::ShutdownHpcMetrics();

    const int port = reserveFreePort();
    ASSERT_GT(port, 0);
    ASSERT_EQ(::setenv("EOP_HPC_METRICS_PORT", std::to_string(port).c_str(), 1), 0);

    hpc::InitHpcMetrics(true);

    hpc::HpcRequestMetrics metrics;
    metrics.status = "ACK";
    metrics.processing_seconds = 0.25;
    metrics.thread_count = 2;
    metrics.graph_nodes = 5;
    metrics.graph_edges = 4;
    hpc::RecordHpcRequestMetric(metrics);

    const std::string response = httpGetMetrics(port);

    ::unsetenv("EOP_HPC_METRICS_PORT");
    hpc::ShutdownHpcMetrics();

    ASSERT_FALSE(response.empty()) << "the exposer did not answer on port " << port;
    EXPECT_NE(response.find("200 OK"), std::string::npos) << response.substr(0, 120);

    // The families the Grafana dashboard depends on must be on the wire, not merely registered.
    EXPECT_NE(response.find("eop_hpc_requests_total"), std::string::npos);
    EXPECT_NE(response.find("eop_hpc_processing_duration_seconds"), std::string::npos);
    EXPECT_NE(response.find("eop_hpc_thread_count"), std::string::npos);
    EXPECT_NE(response.find("eop_hpc_active_threads"), std::string::npos);
    EXPECT_NE(response.find("eop_hpc_thread_busy_seconds_total"), std::string::npos);
}

TEST(HpcMetricsExposerTest, InitIsIdempotentAndKeepsTheExistingRegistry)
{
    hpc::ShutdownHpcMetrics();

    hpc::InitHpcMetrics(false);
    const auto* firstRegistry = hpc::hpcMetricsRegistry.registry.get();

    // A second init must be a no-op: re-registering the families on a fresh registry would
    // silently discard every counter accumulated so far.
    hpc::InitHpcMetrics(false);

    EXPECT_EQ(hpc::hpcMetricsRegistry.registry.get(), firstRegistry);

    hpc::ShutdownHpcMetrics();
}

TEST(HpcMetricsExposerTest, SurvivesAMetricsPortThatIsAlreadyTaken)
{
    hpc::ShutdownHpcMetrics();

    // Hold the port for the duration of the test so the exposer cannot bind it.
    const int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(sock, 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = 0;
    ASSERT_EQ(::bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    ASSERT_EQ(::listen(sock, 1), 0);

    socklen_t length = sizeof(addr);
    ASSERT_EQ(::getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &length), 0);
    ASSERT_EQ(::setenv("EOP_HPC_METRICS_PORT", std::to_string(ntohs(addr.sin_port)).c_str(), 1), 0);

    testing::internal::CaptureStderr();
    hpc::InitHpcMetrics(true);
    const std::string logged = testing::internal::GetCapturedStderr();

    ::unsetenv("EOP_HPC_METRICS_PORT");
    ::close(sock);

    // Losing the scrape endpoint must degrade observability, not stop the engine: the registry
    // is still built and still records, so metrics resume as soon as the port frees up.
    EXPECT_EQ(hpc::hpcMetricsRegistry.exposer, nullptr);
    ASSERT_NE(hpc::hpcMetricsRegistry.registry, nullptr);
    EXPECT_NE(logged.find("Failed to start HPC Prometheus /metrics exposer"), std::string::npos) << logged;

    hpc::HpcRequestMetrics metrics;
    metrics.status = "ACK";
    metrics.processing_seconds = 0.1;
    metrics.thread_count = 1;
    metrics.graph_nodes = 2;
    metrics.graph_edges = 1;
    hpc::RecordHpcRequestMetric(metrics);

    const auto families = hpc::hpcMetricsRegistry.registry->Collect();
    EXPECT_NE(findFamily(families, "eop_hpc_requests_total"), nullptr);

    hpc::ShutdownHpcMetrics();
}

// Each algorithm's parallel region carries its own scope, so each one needs its own check: the
// test above runs connected_components alongside centrality, and connected_components alone is
// enough to make it pass. That blind spot is not hypothetical -- centrality.cpp was rewritten on
// one branch while the scope was added to the old version on another, and resolving that merge by
// keeping the rewrite silently drops the instrumentation from the heaviest algorithm.
TEST_F(HpcMetricsTest, CentralityAloneReportsItsWorkersAsBusy)
{
    const std::string request = R"({"type":"ANALYZE_GRAPH","request_id":"ac6-centrality",)"
                                R"("payload":{"nodes":["a","b","c","d"],)"
                                R"("edges":[{"from":"a","to":"b","weight":1},)"
                                R"({"from":"b","to":"c","weight":1},)"
                                R"({"from":"c","to":"d","weight":1}],)"
                                R"("algorithms":["centrality"]}})";

    testing::internal::CaptureStderr();
    const auto result = hpc::processAnalyzeGraphRequest(request);
    testing::internal::GetCapturedStderr();
    ASSERT_EQ(result.exit_code, 0) << result.payload;

    const auto families = hpc::hpcMetricsRegistry.registry->Collect();
    const auto* busy = findFamily(families, "eop_hpc_thread_busy_seconds_total");
    ASSERT_NE(busy, nullptr);
    const auto* busySample = findSample(*busy, {{"service", "hpc-engine"}});
    ASSERT_NE(busySample, nullptr);
    EXPECT_GT(busySample->counter.value, 0.0) << "centrality's parallel region is not instrumented";
}

TEST_F(HpcMetricsTest, ShortestPathsAloneReportsItsWorkersAsBusy)
{
    const std::string request = R"({"type":"ANALYZE_GRAPH","request_id":"ac6-shortest",)"
                                R"("payload":{"nodes":["a","b","c"],)"
                                R"("edges":[{"from":"a","to":"b","weight":1},)"
                                R"({"from":"b","to":"c","weight":1}],)"
                                R"("algorithms":["shortest_paths"]}})";

    testing::internal::CaptureStderr();
    const auto result = hpc::processAnalyzeGraphRequest(request);
    testing::internal::GetCapturedStderr();
    ASSERT_EQ(result.exit_code, 0) << result.payload;

    const auto families = hpc::hpcMetricsRegistry.registry->Collect();
    const auto* busy = findFamily(families, "eop_hpc_thread_busy_seconds_total");
    ASSERT_NE(busy, nullptr);
    const auto* busySample = findSample(*busy, {{"service", "hpc-engine"}});
    ASSERT_NE(busySample, nullptr);
    EXPECT_GT(busySample->counter.value, 0.0) << "shortest_paths' parallel region is not instrumented";
}
