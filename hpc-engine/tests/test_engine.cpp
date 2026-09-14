#include <string>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "hpc/engine.hpp"

namespace
{

const std::string TRACEPARENT = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
const std::string TRACE_ID = "4bf92f3577b34da6a3ce929d0e0e4736";

std::string buildRequest(const std::string& traceparent)
{
    nlohmann::json request;
    request["type"] = "ANALYZE_GRAPH";
    request["request_id"] = "log-test";
    if (!traceparent.empty())
    {
        request["traceparent"] = traceparent;
    }
    request["payload"]["nodes"] = {"a", "b", "c"};
    request["payload"]["edges"] = {{{"from", "a"}, {"to", "b"}, {"weight", 1}},
                                   {{"from", "b"}, {"to", "c"}, {"weight", 1}}};
    request["payload"]["algorithms"] = {"connected_components", "centrality"};

    return request.dump();
}

// The engine writes exactly one structured log line per request; anything else on stderr
// would be a bug, so the test parses the captured output as a single JSON object.
nlohmann::json runAndCaptureLog(const std::string& request)
{
    testing::internal::CaptureStderr();
    const auto result = hpc::processAnalyzeGraphRequest(request);
    const std::string captured = testing::internal::GetCapturedStderr();

    EXPECT_EQ(result.exit_code, 0) << result.payload;

    return nlohmann::json::parse(captured);
}

} // namespace

// US-201 AC5: "The engine logs per request: graph size received, algorithms executed,
// wall-clock processing time, thread count."
TEST(EngineRequestLoggingTest, LogsGraphSizeAlgorithmsWallClockAndThreadCount)
{
    const auto entry = runAndCaptureLog(buildRequest(""));

    EXPECT_EQ(entry["event"], "analyze_graph_completed");
    EXPECT_EQ(entry["request_id"], "log-test");

    EXPECT_EQ(entry["graph_nodes"], 3);
    EXPECT_EQ(entry["graph_edges"], 2);

    EXPECT_EQ(entry["algorithms"], nlohmann::json::array({"connected_components", "centrality"}));

    ASSERT_TRUE(entry["processing_ms"].is_number());
    EXPECT_GE(entry["processing_ms"].get<double>(), 0.0);

    ASSERT_TRUE(entry["thread_count"].is_number_integer());
    EXPECT_GE(entry["thread_count"].get<int>(), 1);
}

// The observability NFR requires structured logs carrying timestamp/level/service, so that a
// collector can read them without a per-service parser.
TEST(EngineRequestLoggingTest, LogEntryCarriesTheStructuredLogEnvelope)
{
    const auto entry = runAndCaptureLog(buildRequest(""));

    EXPECT_EQ(entry["level"], "info");
    EXPECT_EQ(entry["service"], "hpc-engine");

    const auto timestamp = entry["timestamp"].get<std::string>();
    EXPECT_EQ(timestamp.size(), 24u) << timestamp;
    EXPECT_EQ(timestamp.back(), 'Z') << timestamp;
    EXPECT_EQ(timestamp[10], 'T') << timestamp;
}

TEST(EngineRequestLoggingTest, CorrelatesTheLogLineWithTheIncomingTrace)
{
    const auto entry = runAndCaptureLog(buildRequest(TRACEPARENT));

    ASSERT_TRUE(entry.contains("trace_id"));
    EXPECT_EQ(entry["trace_id"], TRACE_ID);
}

TEST(EngineRequestLoggingTest, OmitsTraceIdWhenTheRequestCarriesNoTraceContext)
{
    const auto entry = runAndCaptureLog(buildRequest(""));

    // Absent rather than empty: an empty trace_id would look like a broken correlation instead
    // of a request that legitimately arrived without trace context.
    EXPECT_FALSE(entry.contains("trace_id"));
}

TEST(EngineRequestLoggingTest, IgnoresATraceparentTooShortToHoldATraceId)
{
    const auto entry = runAndCaptureLog(buildRequest("00-tooshort"));

    EXPECT_FALSE(entry.contains("trace_id"));
}

// --- US-201 AC6 / US-203 error contract ----------------------------------------------------
// "Invalid or malformed graph payloads return a structured error response, not a server
// crash." The individual validators have their own tests; what these cover is the engine
// entry point itself -- that each rejection path produces a well-formed response envelope and
// a non-zero exit code instead of throwing out of the module boundary.

namespace
{

nlohmann::json runExpectingError(const std::string& request)
{
    testing::internal::CaptureStderr();
    const auto result = hpc::processAnalyzeGraphRequest(request);
    testing::internal::GetCapturedStderr();

    EXPECT_NE(result.exit_code, 0) << "a rejected request must not report success";
    EXPECT_FALSE(result.payload.empty());
    EXPECT_EQ(result.payload.back(), '\n') << "responses are newline-delimited (ADR-003)";

    return nlohmann::json::parse(result.payload);
}

} // namespace

TEST(EngineErrorContractTest, EmptyInputIsRejectedAsAnInvalidRequest)
{
    const auto response = runExpectingError("");

    EXPECT_EQ(response["status"], "ERROR");
    EXPECT_EQ(response["error_code"], "ERROR_INVALID_REQUEST");
    EXPECT_FALSE(response["retryable"].get<bool>());
}

TEST(EngineErrorContractTest, MalformedJsonIsRejectedWithoutThrowing)
{
    const auto response = runExpectingError(R"({"type":"ANALYZE_GRAPH",)");

    EXPECT_EQ(response["status"], "ERROR");
    EXPECT_EQ(response["error_code"], "ERROR_INVALID_REQUEST");
}

TEST(EngineErrorContractTest, ParseFailureEchoesTheRequestIdWhenOneWasReadable)
{
    // The request_id parses fine; the payload does not. Echoing it back is what lets a client
    // correlate the rejection with the request it sent.
    const auto response = runExpectingError(R"({"type":"ANALYZE_GRAPH","request_id":"rid-42"})");

    EXPECT_EQ(response["status"], "ERROR");
    EXPECT_EQ(response["request_id"], "rid-42");
    EXPECT_EQ(response["field"], "payload");
}

TEST(EngineErrorContractTest, AGraphRejectedByValidationReturnsAStructuredError)
{
    nlohmann::json request;
    request["type"] = "ANALYZE_GRAPH";
    request["request_id"] = "bad-edge";
    request["payload"]["nodes"] = {"a", "b"};
    // Edge referencing a node that is not declared in the node list.
    request["payload"]["edges"] = {{{"from", "a"}, {"to", "ghost"}, {"weight", 1}}};
    request["payload"]["algorithms"] = {"connected_components"};

    const auto response = runExpectingError(request.dump());

    EXPECT_EQ(response["status"], "ERROR");
    EXPECT_EQ(response["request_id"], "bad-edge");
    EXPECT_TRUE(response.contains("error_code"));
    EXPECT_TRUE(response.contains("message"));
}

TEST(EngineErrorContractTest, AnUnsupportedAlgorithmIsRejected)
{
    nlohmann::json request;
    request["type"] = "ANALYZE_GRAPH";
    request["request_id"] = "bad-algo";
    request["payload"]["nodes"] = {"a", "b"};
    request["payload"]["edges"] = {{{"from", "a"}, {"to", "b"}, {"weight", 1}}};
    request["payload"]["algorithms"] = {"pagerank"};

    const auto response = runExpectingError(request.dump());

    EXPECT_EQ(response["error_code"], "ERROR_UNSUPPORTED_ALGORITHM");
}

// shortest_paths is dispatched by the engine but, until now, only ever exercised by calling
// computeShortestPaths() directly -- the engine's own branch for it was never executed.
TEST(EngineRequestDispatchTest, DispatchesShortestPathsAndReturnsItsResult)
{
    nlohmann::json request;
    request["type"] = "ANALYZE_GRAPH";
    request["request_id"] = "sp";
    request["payload"]["nodes"] = {"a", "b", "c"};
    request["payload"]["edges"] = {{{"from", "a"}, {"to", "b"}, {"weight", 1}},
                                   {{"from", "b"}, {"to", "c"}, {"weight", 2}}};
    request["payload"]["algorithms"] = {"shortest_paths"};

    testing::internal::CaptureStderr();
    const auto result = hpc::processAnalyzeGraphRequest(request.dump());
    const std::string log = testing::internal::GetCapturedStderr();

    ASSERT_EQ(result.exit_code, 0) << result.payload;

    const auto response = nlohmann::json::parse(result.payload);
    EXPECT_EQ(response["status"], "ACK");
    ASSERT_TRUE(response["result"]["algorithms"].contains("shortest_paths"));
    EXPECT_DOUBLE_EQ(response["result"]["algorithms"]["shortest_paths"]["a-c"]["cost"].get<double>(), 3.0);
    EXPECT_EQ(response["result"]["executed_algorithms"], nlohmann::json::array({"shortest_paths"}));

    // The per-request log must name the algorithm that actually ran (US-201 AC5).
    const auto entry = nlohmann::json::parse(log);
    EXPECT_EQ(entry["algorithms"], nlohmann::json::array({"shortest_paths"}));
}
