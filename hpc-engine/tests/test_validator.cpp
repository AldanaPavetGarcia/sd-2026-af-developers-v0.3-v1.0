/**
 * @file test_validator.cpp
 * @brief Unit tests for the graph validation layer.
 *
 * Covers: empty nodes, duplicate nodes, invalid edge refs, negative/NaN/Inf weights,
 * unsupported algorithms, and full JSON input path for invalid edge references.
 */

#include <cmath>
#include <limits>
#include <string>

#include <gtest/gtest.h>

#include "hpc/json_parser.hpp"
#include "hpc/validator.hpp"

// Helper to build a simple valid graph
static hpc::Graph makeValidGraph()
{
    hpc::Graph g;
    g.num_nodes = 3;
    g.node_names = {"A", "B", "C"};
    g.node_index = {{"A", 0}, {"B", 1}, {"C", 2}};
    g.adjacency.resize(3);
    g.adjacency[0].push_back({1, 10.0});
    g.adjacency[1].push_back({2, 5.0});
    g.num_edges = 2;
    return g;
}

TEST(ValidatorTest, ValidGraphPasses)
{
    auto g = makeValidGraph();
    auto r = hpc::validateGraph(g, {"shortest_paths", "connected_components", "centrality"});
    EXPECT_TRUE(r.ok);
}

TEST(ValidatorTest, EmptyNodesListFails)
{
    hpc::Graph g;
    g.num_nodes = 0;
    auto r = hpc::validateGraph(g, {"shortest_paths"});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error_code, "ERROR_INVALID_GRAPH");
}

TEST(ValidatorTest, DuplicateNodesFails)
{
    hpc::Graph g;
    g.num_nodes = 2;
    g.node_names = {"A", "A"};
    g.node_index = {{"A", 0}};
    g.adjacency.resize(2);
    auto r = hpc::validateGraph(g, {"shortest_paths"});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error_code, "ERROR_INVALID_GRAPH");
}

TEST(ValidatorTest, NegativeWeightFails)
{
    auto g = makeValidGraph();
    g.adjacency[0][0].weight = -1.0;
    auto r = hpc::validateGraph(g, {"shortest_paths"});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error_code, "ERROR_INVALID_GRAPH");
}

TEST(ValidatorTest, ZeroWeightFails)
{
    auto g = makeValidGraph();
    g.adjacency[0][0].weight = 0.0;
    auto r = hpc::validateGraph(g, {"shortest_paths"});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error_code, "ERROR_INVALID_GRAPH");
}

TEST(ValidatorTest, NaNWeightFails)
{
    auto g = makeValidGraph();
    g.adjacency[0][0].weight = std::numeric_limits<double>::quiet_NaN();
    auto r = hpc::validateGraph(g, {"shortest_paths"});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error_code, "ERROR_INVALID_GRAPH");
}

TEST(ValidatorTest, InfinityWeightFails)
{
    auto g = makeValidGraph();
    g.adjacency[0][0].weight = std::numeric_limits<double>::infinity();
    auto r = hpc::validateGraph(g, {"shortest_paths"});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error_code, "ERROR_INVALID_GRAPH");
}

TEST(ValidatorTest, UnsupportedAlgorithmFails)
{
    auto g = makeValidGraph();
    auto r = hpc::validateGraph(g, {"shortest_paths", "unknown_algo"});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error_code, "ERROR_UNSUPPORTED_ALGORITHM");
}

TEST(ValidatorTest, InvalidEdgeTargetFails)
{
    hpc::Graph g;
    g.num_nodes = 2;
    g.node_names = {"A", "B"};
    g.node_index = {{"A", 0}, {"B", 1}};
    g.adjacency.resize(2);
    g.adjacency[0].push_back({99, 1.0}); // target index out of bounds
    g.num_edges = 1;
    auto r = hpc::validateGraph(g, {"shortest_paths"});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error_code, "ERROR_INVALID_GRAPH");
}

TEST(ValidatorTest, SingleNodeGraphIsValid)
{
    hpc::Graph g;
    g.num_nodes = 1;
    g.node_names = {"solo"};
    g.node_index = {{"solo", 0}};
    g.adjacency.resize(1);
    g.num_edges = 0;
    auto r = hpc::validateGraph(g, {"connected_components"});
    EXPECT_TRUE(r.ok);
}

// ---------------------------------------------------------------------------
// Full JSON input path integration test
// ---------------------------------------------------------------------------

/**
 * @test ParseRequest_InvalidEdgeNodeReference
 *
 * Verifies that parseRequest() rejects a well-formed JSON request in which an
 * edge references a node ("Z") that is not declared in payload.nodes.
 * This exercises the complete ingestion path (not the validator in isolation)
 * and ensures the engine never silently accepts or drops the invalid edge.
 */
TEST(ValidatorTest, ParseRequest_InvalidEdgeNodeReference)
{
    // Exact payload from the reviewer's bug report
    const std::string raw_json = R"({
        "type": "ANALYZE_GRAPH",
        "request_id": "manual-invalid-001",
        "payload": {
            "nodes": ["A", "B"],
            "edges": [
                {"from": "A", "to": "Z", "weight": 3}
            ],
            "algorithms": ["shortest_paths"]
        }
    })";

    auto result = hpc::parseRequest(raw_json);

    EXPECT_FALSE(result.ok)
        << "Engine must not return ACK for an edge referencing undeclared node 'Z'";
    EXPECT_EQ(result.error_code, "ERROR_INVALID_GRAPH")
        << "Error code must be ERROR_INVALID_GRAPH, got: " << result.error_code;
    EXPECT_FALSE(result.error_message.empty())
        << "A descriptive error message must be present";
    // The offending field should point into edges[0].to
    EXPECT_EQ(result.error_field, "payload.edges[0].to")
        << "Error field must identify the exact edge endpoint";
}

// ---------------------------------------------------------------------------
// Comprehensive JSON Parser Coverage Tests
// ---------------------------------------------------------------------------

TEST(ValidatorTest, ParseRequest_InvalidJSON)
{
    auto r = hpc::parseRequest("{ invalid_json ");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error_code, "ERROR_INVALID_REQUEST");
}

TEST(ValidatorTest, ParseRequest_MissingType)
{
    auto r = hpc::parseRequest(R"({"request_id": "123", "payload": {}})");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error_field, "type");
}

TEST(ValidatorTest, ParseRequest_MissingRequestId)
{
    auto r = hpc::parseRequest(R"({"type": "ANALYZE_GRAPH", "payload": {}})");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error_field, "request_id");
}

TEST(ValidatorTest, ParseRequest_TraceContext)
{
    auto r = hpc::parseRequest(R"({
        "type": "ANALYZE_GRAPH",
        "request_id": "1",
        "traceparent": "00-123-456-01",
        "tracestate": "rojo=1",
        "payload": {
            "nodes": [], "edges": [], "algorithms": []
        }
    })");
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.request.traceparent, "00-123-456-01");
    EXPECT_EQ(r.request.tracestate, "rojo=1");
}

TEST(ValidatorTest, ParseRequest_MissingPayload)
{
    auto r = hpc::parseRequest(R"({"type": "ANALYZE_GRAPH", "request_id": "1"})");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error_field, "payload");
}

TEST(ValidatorTest, ParseRequest_MissingNodes)
{
    auto r = hpc::parseRequest(R"({"type": "ANALYZE_GRAPH", "request_id": "1", "payload": {}})");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error_field, "payload.nodes");
}

TEST(ValidatorTest, ParseRequest_NodeNotString)
{
    auto r = hpc::parseRequest(R"({"type": "ANALYZE_GRAPH", "request_id": "1", "payload": {"nodes": [123]}})");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error_field, "payload.nodes[0]");
}

TEST(ValidatorTest, ParseRequest_MissingEdges)
{
    auto r = hpc::parseRequest(R"({"type": "ANALYZE_GRAPH", "request_id": "1", "payload": {"nodes": []}})");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error_field, "payload.edges");
}

TEST(ValidatorTest, ParseRequest_EdgeNotObject)
{
    auto r = hpc::parseRequest(R"({"type": "ANALYZE_GRAPH", "request_id": "1", "payload": {"nodes": [], "edges": [123]}})");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error_field, "payload.edges[0]");
}

TEST(ValidatorTest, ParseRequest_EdgeMissingFrom)
{
    auto r = hpc::parseRequest(R"({"type": "ANALYZE_GRAPH", "request_id": "1", "payload": {"nodes": [], "edges": [{}]}})");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error_field, "payload.edges[0].from");
}

TEST(ValidatorTest, ParseRequest_EdgeMissingTo)
{
    auto r = hpc::parseRequest(R"({"type": "ANALYZE_GRAPH", "request_id": "1", "payload": {"nodes": [], "edges": [{"from": "A"}]}})");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error_field, "payload.edges[0].to");
}

TEST(ValidatorTest, ParseRequest_EdgeMissingWeight)
{
    auto r = hpc::parseRequest(R"({"type": "ANALYZE_GRAPH", "request_id": "1", "payload": {"nodes": [], "edges": [{"from": "A", "to": "B"}]}})");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error_field, "payload.edges[0].weight");
}

TEST(ValidatorTest, ParseRequest_EdgeInvalidFromRef)
{
    auto r = hpc::parseRequest(R"({"type": "ANALYZE_GRAPH", "request_id": "1", "payload": {"nodes": ["B"], "edges": [{"from": "A", "to": "B", "weight": 1.0}]}})");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error_code, "ERROR_INVALID_GRAPH");
    EXPECT_EQ(r.error_field, "payload.edges[0].from");
}

TEST(ValidatorTest, ParseRequest_MissingAlgorithms)
{
    auto r = hpc::parseRequest(R"({"type": "ANALYZE_GRAPH", "request_id": "1", "payload": {"nodes": [], "edges": []}})");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error_field, "payload.algorithms");
}

TEST(ValidatorTest, ParseRequest_AlgorithmNotString)
{
    auto r = hpc::parseRequest(R"({"type": "ANALYZE_GRAPH", "request_id": "1", "payload": {"nodes": [], "edges": [], "algorithms": [123]}})");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error_field, "payload.algorithms");
}
