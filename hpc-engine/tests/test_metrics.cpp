#include "hpc/metrics.hpp"

#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

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
