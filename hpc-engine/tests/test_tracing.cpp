#include <array>
#include <cstdlib>
#include <string>

#include <gtest/gtest.h>

#include <opentelemetry/nostd/span.h>
#include <opentelemetry/sdk/trace/tracer_provider.h>
#include <opentelemetry/trace/provider.h>

#include "hpc/tracing.hpp"

namespace
{

    const std::string TRACE_ID = "4bf92f3577b34da6a3ce929d0e0e4736";
    const std::string SPAN_ID = "00f067aa0ba902b7";
    const std::string TRACEPARENT = "00-" + TRACE_ID + "-" + SPAN_ID + "-01";
    // Nothing listens here: spans are created and ended for real, and the export simply fails.
    const char* UNREACHABLE_ENDPOINT = "http://127.0.0.1:1/v1/traces";

    std::string traceIdOf(const opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span>& span)
    {
        std::array<char, 32> buf {};
        span->GetContext().trace_id().ToLowerBase16(opentelemetry::nostd::span<char, 32>(buf.data(), buf.size()));
        return {buf.data(), buf.size()};
    }

    bool sdkProviderInstalled()
    {
        auto provider = opentelemetry::trace::Provider::GetTracerProvider();
        return dynamic_cast<opentelemetry::sdk::trace::TracerProvider*>(provider.get()) != nullptr;
    }

    class HpcTracingTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            unsetenv("EOP_TRACING_ENABLED");
            setenv("OTEL_EXPORTER_OTLP_ENDPOINT", UNREACHABLE_ENDPOINT, 1);
        }

        void TearDown() override
        {
            hpc::ShutdownHpcTracer();
            unsetenv("EOP_TRACING_ENABLED");
            unsetenv("OTEL_EXPORTER_OTLP_ENDPOINT");
        }
    };

} // namespace

TEST_F(HpcTracingTest, DisabledTracingInstallsNoProvider)
{
    setenv("EOP_TRACING_ENABLED", "0", 1);
    hpc::InitHpcTracer();
    EXPECT_FALSE(sdkProviderInstalled());
}

TEST_F(HpcTracingTest, AnyOtherSwitchValueEnablesTracing)
{
    setenv("EOP_TRACING_ENABLED", "1", 1);
    hpc::InitHpcTracer();
    EXPECT_TRUE(sdkProviderInstalled());
}

TEST_F(HpcTracingTest, FallsBackToTheDefaultEndpoint)
{
    unsetenv("OTEL_EXPORTER_OTLP_ENDPOINT");
    hpc::InitHpcTracer();
    EXPECT_TRUE(sdkProviderInstalled());
}

TEST_F(HpcTracingTest, ShutdownRemovesTheProvider)
{
    hpc::InitHpcTracer();
    ASSERT_TRUE(sdkProviderInstalled());
    hpc::ShutdownHpcTracer();
    EXPECT_FALSE(sdkProviderInstalled());
}

// US-203 AC5: the engine's span joins the trace that eop-server forwarded.
TEST_F(HpcTracingTest, EngineSpanContinuesTheForwardedTrace)
{
    hpc::InitHpcTracer();
    auto span = hpc::StartAnalyzeGraphSpan(TRACEPARENT, "req", 3, 2);
    EXPECT_EQ(traceIdOf(span), TRACE_ID);
    auto child = hpc::StartAlgorithmSpan(span, "centrality");
    EXPECT_EQ(traceIdOf(child), TRACE_ID);
    child->End();
    span->End();
}

// A garbled traceparent must not cost the engine its own span: it starts a new trace instead.
TEST_F(HpcTracingTest, MalformedTraceparentStartsANewTrace)
{
    hpc::InitHpcTracer();
    const std::string malformed[] = {
        "",
        "00-" + TRACE_ID + "-" + SPAN_ID + "-0",                  // wrong length
        "00_" + TRACE_ID + "-" + SPAN_ID + "-01",                 // wrong separator
        "00-4BF92F3577B34DA6A3CE929D0E0E4736-" + SPAN_ID + "-01", // uppercase hex
        "00-00000000000000000000000000000000-" + SPAN_ID + "-01", // zero trace-id
        "00-" + TRACE_ID + "-0000000000000000-01",                // zero span-id
    };
    for (const auto& value : malformed)
    {
        auto span = hpc::StartAnalyzeGraphSpan(value, "req", 1, 0);
        EXPECT_TRUE(span->GetContext().IsValid()) << value;
        EXPECT_NE(traceIdOf(span), TRACE_ID) << value;
        span->End();
    }
}
