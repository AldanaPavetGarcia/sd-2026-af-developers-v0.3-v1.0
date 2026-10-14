#include "../../lib/include/traceContext.hpp"

#include <array>
#include <gtest/gtest.h>
#include <opentelemetry/nostd/span.h>

namespace
{
    constexpr const char* VALID = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";

    std::string hexOf(const opentelemetry::trace::TraceId& id)
    {
        std::array<char, 32> buf {};
        id.ToLowerBase16(opentelemetry::nostd::span<char, 32>(buf.data(), 32));
        return {buf.data(), buf.size()};
    }

    std::string hexOf(const opentelemetry::trace::SpanId& id)
    {
        std::array<char, 16> buf {};
        id.ToLowerBase16(opentelemetry::nostd::span<char, 16>(buf.data(), 16));
        return {buf.data(), buf.size()};
    }
} // namespace

TEST(TraceContextTest, ParsesAValidTraceparent)
{
    const auto ctx = parseTraceparent(VALID);
    ASSERT_TRUE(ctx.IsValid());
    EXPECT_TRUE(ctx.IsRemote());
    EXPECT_EQ(hexOf(ctx.trace_id()), "4bf92f3577b34da6a3ce929d0e0e4736");
    EXPECT_EQ(hexOf(ctx.span_id()), "00f067aa0ba902b7");
    EXPECT_TRUE(ctx.IsSampled());
}

TEST(TraceContextTest, KeepsTheSampledFlag)
{
    EXPECT_FALSE(parseTraceparent("00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-00").IsSampled());
}

TEST(TraceContextTest, RejectsMalformedValues)
{
    const char* malformed[] = {
        "",
        "garbage",
        "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-0",   // too short
        "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-011", // too long
        "00_4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01",  // wrong separator
        "00-4bf92f3577b34da6a3ce929d0e0e4736_00f067aa0ba902b7-01",  // wrong separator
        "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7_01",  // wrong separator
        "ff-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01",  // forbidden version
        "00-4BF92F3577B34DA6A3CE929D0E0E4736-00f067aa0ba902b7-01",  // uppercase
        "00-00000000000000000000000000000000-00f067aa0ba902b7-01",  // zero trace-id
        "00-4bf92f3577b34da6a3ce929d0e0e4736-0000000000000000-01",  // zero span-id
        "zz-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01",  // non-hex version
        "00-4bf92f3577b34da6a3ce929d0e0e47zz-00f067aa0ba902b7-01",  // non-hex trace-id
        "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902zz-01",  // non-hex span-id
        "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-0g",  // non-hex flags
    };
    for (const char* value : malformed)
    {
        EXPECT_FALSE(parseTraceparent(value).IsValid()) << value;
    }
}

TEST(TraceContextTest, ExtractsTheKeyFromTextCommands)
{
    EXPECT_EQ(extractTraceparentKey(std::string("QUERY_NODE node_id=a traceparent=") + VALID), VALID);
    EXPECT_EQ(extractTraceparentKey(std::string("LIST_NODES traceparent=") + VALID), VALID);
    EXPECT_EQ(extractTraceparentKey(std::string("traceparent=") + VALID + " node_id=a"), VALID);
    EXPECT_EQ(extractTraceparentKey("QUERY_NODE node_id=a"), "");
    EXPECT_EQ(extractTraceparentKey("LIST_NODES"), "");
    EXPECT_EQ(extractTraceparentKey("QUERY_NODE node_id=atraceparent=x"), "") << "the key must start a field";
    EXPECT_EQ(extractTraceparentKey("QUERY_NODE x_traceparent=1 traceparent=abc"), "abc");
    EXPECT_EQ(extractTraceparentKey("QUERY_NODE traceparent="), "");
}
