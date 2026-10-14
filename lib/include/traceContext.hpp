/**
 * @file traceContext.hpp
 * @brief W3C trace context carried by EOP protocol messages.
 * @details Text commands may end with an optional `traceparent=<W3C traceparent>` key and the
 *          ANALYZE_GRAPH JSON envelope may carry a `traceparent` field (ADR-026, ADR-015). When
 *          the value is valid, the server starts the command's span as a child of the caller's
 *          span, so a request that goes through the API gateway is one trace end to end. An
 *          absent or malformed value is ignored and the server behaves as before.
 */
#ifndef TRACE_CONTEXT_HPP
#define TRACE_CONTEXT_HPP

#include <opentelemetry/trace/span_context.h>
#include <string>
#include <string_view>

/**
 * @brief Returns the value of the optional `traceparent=` key of a text command.
 * @details Keys are separated by single spaces, as in every v0.1 text command; the key is
 *          expected last but is found anywhere in the line. Only the value is returned, not
 *          validated; see parseTraceparent().
 * @param line One text command, without its trailing newline.
 * @return The value after `traceparent=`, or an empty string when the key is absent.
 */
std::string extractTraceparentKey(std::string_view line);

/**
 * @brief Parses a W3C `traceparent` value into a remote span context.
 * @details Accepts `<version>-<32 hex trace-id>-<16 hex span-id>-<2 hex flags>` in lowercase hex,
 *          rejects version `ff`, an all-zero trace-id or span-id, and any other length or
 *          character, as the W3C Trace Context specification requires.
 * @param value The header value.
 * @return A valid, remote SpanContext, or an invalid one (`IsValid() == false`) when the value is
 *         empty or malformed.
 * @post The returned context, if valid, has `IsRemote() == true`.
 */
opentelemetry::trace::SpanContext parseTraceparent(std::string_view value);

#endif
