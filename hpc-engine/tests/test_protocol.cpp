/**
 * @file test_protocol.cpp
 * @brief Unit tests for JSON response builders.
 */

#include <gtest/gtest.h>

#include "hpc/protocol.hpp"

TEST(ProtocolTest, BuildAckResponse)
{
    nlohmann::json algo_res = {
        {"shortest_paths", nlohmann::json::object()}
    };

    nlohmann::json executed_algorithms = nlohmann::json::array({"shortest_paths"});

    nlohmann::json algorithm_timings_ms = {
        {"shortest_paths", 12.75}
    };

    auto res = hpc::buildAckResponse(
        "req-123",
        "trace-xyz",
        10,
        15,
        42.5,
        4,
        algo_res,
        executed_algorithms,
        algorithm_timings_ms);

    EXPECT_EQ(res["type"], "RESPONSE");
    EXPECT_EQ(res["status"], "ACK");
    EXPECT_EQ(res["request_id"], "req-123");
    EXPECT_EQ(res["traceparent"], "trace-xyz");
    EXPECT_EQ(res["result"]["graph_size"]["nodes"], 10);
    EXPECT_EQ(res["result"]["graph_size"]["edges"], 15);
    EXPECT_DOUBLE_EQ(res["result"]["processing_time_ms"], 42.5);
    EXPECT_EQ(res["result"]["thread_count"], 4);
    EXPECT_TRUE(res["result"]["algorithms"].contains("shortest_paths"));

    ASSERT_TRUE(res["result"].contains("executed_algorithms"));
    ASSERT_TRUE(res["result"]["executed_algorithms"].is_array());
    ASSERT_EQ(res["result"]["executed_algorithms"].size(), 1);
    EXPECT_EQ(res["result"]["executed_algorithms"][0], "shortest_paths");

    ASSERT_TRUE(res["result"].contains("algorithm_timings_ms"));
    ASSERT_TRUE(res["result"]["algorithm_timings_ms"].contains("shortest_paths"));
    EXPECT_DOUBLE_EQ(res["result"]["algorithm_timings_ms"]["shortest_paths"], 12.75);
}

TEST(ProtocolTest, BuildErrorResponse)
{
    auto res = hpc::buildErrorResponse(
        "req-456", "ERROR_CODE", "Error message", "payload.field", true);

    EXPECT_EQ(res["type"], "RESPONSE");
    EXPECT_EQ(res["status"], "ERROR");
    EXPECT_EQ(res["request_id"], "req-456");
    EXPECT_EQ(res["error_code"], "ERROR_CODE");
    EXPECT_EQ(res["message"], "Error message");
    EXPECT_EQ(res["field"], "payload.field");
    EXPECT_TRUE(res["retryable"]);
}

TEST(ProtocolTest, BuildErrorResponseNoOptionalFields)
{
    auto res = hpc::buildErrorResponse("", "ERROR_CODE", "Message");

    EXPECT_EQ(res["type"], "RESPONSE");
    EXPECT_EQ(res["status"], "ERROR");
    EXPECT_FALSE(res.contains("request_id"));
    EXPECT_FALSE(res.contains("field"));
    EXPECT_FALSE(res["retryable"]);
}
