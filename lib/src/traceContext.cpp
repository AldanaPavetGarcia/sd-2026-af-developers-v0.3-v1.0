#include "traceContext.hpp"

#include <array>
#include <cstdint>
#include <opentelemetry/nostd/span.h>
#include <opentelemetry/trace/span_id.h>
#include <opentelemetry/trace/trace_flags.h>
#include <opentelemetry/trace/trace_id.h>

namespace
{
    constexpr std::string_view TRACEPARENT_KEY = "traceparent=";
    constexpr std::size_t TRACEPARENT_LENGTH = 55; // 2 + 1 + 32 + 1 + 16 + 1 + 2
    constexpr std::size_t VERSION_LENGTH = 2;
    constexpr std::size_t TRACE_ID_OFFSET = 3;
    constexpr std::size_t TRACE_ID_HEX_LENGTH = 32;
    constexpr std::size_t SPAN_ID_OFFSET = 36;
    constexpr std::size_t SPAN_ID_HEX_LENGTH = 16;
    constexpr std::size_t FLAGS_OFFSET = 53;
    constexpr std::size_t FLAGS_HEX_LENGTH = 2;
    constexpr std::string_view INVALID_VERSION = "ff";
    constexpr int HEX_BASE = 10;
    constexpr int NIBBLE_BITS = 4;

    int hexValue(char c)
    {
        if (c >= '0' && c <= '9')
        {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f')
        {
            return c - 'a' + HEX_BASE;
        }
        return -1; // uppercase is not allowed by the W3C specification
    }

    // Decodes exactly out.size() bytes from 2 * out.size() lowercase hex characters.
    template<std::size_t N>
    bool decodeHex(std::string_view hex, std::array<uint8_t, N>& out)
    {
        for (std::size_t i = 0; i < N; ++i)
        {
            const int high = hexValue(hex[2 * i]);
            const int low = hexValue(hex[2 * i + 1]);
            if (high < 0 || low < 0)
            {
                return false;
            }
            out[i] = static_cast<uint8_t>((high << NIBBLE_BITS) | low);
        }
        return true;
    }

    template<std::size_t N>
    bool allZero(const std::array<uint8_t, N>& bytes)
    {
        for (uint8_t b : bytes)
        {
            if (b != 0)
            {
                return false;
            }
        }
        return true;
    }
} // namespace

std::string extractTraceparentKey(std::string_view line)
{
    std::size_t pos = 0;
    while ((pos = line.find(TRACEPARENT_KEY, pos)) != std::string_view::npos)
    {
        // The key must start a field: at the beginning of the line or after a space.
        if (pos == 0 || line[pos - 1] == ' ')
        {
            const std::size_t start = pos + TRACEPARENT_KEY.size();
            const std::size_t end = line.find(' ', start);
            return std::string(
                line.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        }
        pos += TRACEPARENT_KEY.size();
    }
    return {};
}

opentelemetry::trace::SpanContext parseTraceparent(std::string_view value)
{
    const auto invalid = opentelemetry::trace::SpanContext::GetInvalid();
    if (value.size() != TRACEPARENT_LENGTH || value[VERSION_LENGTH] != '-' || value[SPAN_ID_OFFSET - 1] != '-' ||
        value[FLAGS_OFFSET - 1] != '-' || value.substr(0, VERSION_LENGTH) == INVALID_VERSION)
    {
        return invalid;
    }

    std::array<uint8_t, 1> version {};
    std::array<uint8_t, TRACE_ID_HEX_LENGTH / 2> traceId {};
    std::array<uint8_t, SPAN_ID_HEX_LENGTH / 2> spanId {};
    std::array<uint8_t, FLAGS_HEX_LENGTH / 2> flags {};
    if (!decodeHex(value.substr(0, VERSION_LENGTH), version) ||
        !decodeHex(value.substr(TRACE_ID_OFFSET, TRACE_ID_HEX_LENGTH), traceId) ||
        !decodeHex(value.substr(SPAN_ID_OFFSET, SPAN_ID_HEX_LENGTH), spanId) ||
        !decodeHex(value.substr(FLAGS_OFFSET, FLAGS_HEX_LENGTH), flags) || allZero(traceId) || allZero(spanId))
    {
        return invalid;
    }

    return opentelemetry::trace::SpanContext(
        opentelemetry::trace::TraceId(
            opentelemetry::nostd::span<const uint8_t, TRACE_ID_HEX_LENGTH / 2>(traceId.data(), traceId.size())),
        opentelemetry::trace::SpanId(
            opentelemetry::nostd::span<const uint8_t, SPAN_ID_HEX_LENGTH / 2>(spanId.data(), spanId.size())),
        opentelemetry::trace::TraceFlags(flags[0]),
        true);
}
