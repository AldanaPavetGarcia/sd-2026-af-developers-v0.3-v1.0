#include "../../lib/include/eventAuditLogger.hpp"
#include <gtest/gtest.h>
#include <unistd.h>

#include <string>

#include <memory>

#include <nlohmann/json.hpp>

#include <opentelemetry/exporters/memory/in_memory_span_exporter.h>
#include <opentelemetry/sdk/trace/simple_processor_factory.h>
#include <opentelemetry/sdk/trace/tracer_provider_factory.h>
#include <opentelemetry/trace/provider.h>
#include <opentelemetry/trace/scope.h>

TEST(EventAuditLoggerTest, WriteLogTypes)
{
    EventAuditLogger logger;
    logger.setLogFile("/tmp/test_system.log");

    EXPECT_GT(logger.writeLog("Test INFO", INFO_LOG), 0);
    EXPECT_GT(logger.writeLog("Test CONTROL", CONTROL_LOG), 0);
    EXPECT_GT(logger.writeLog("Test ERROR", ERROR_LOG), 0);
    EXPECT_GT(logger.writeLog("Test WARNING", WARNING_LOG), 0);
    EXPECT_GT(logger.writeLog("Test CRITICAL", CRITICAL_LOG), 0);
    EXPECT_GT(logger.writeLog("Test EVENT", EVENT_LOG), 0);
    EXPECT_GT(logger.writeLog("Test DEBUG", DEBUG_LOG), 0);
    EXPECT_GT(logger.writeLog("Test DEFAULT", (LogType)999), 0); // NOLINT

    // CONNECTION_LOG should also trigger writeToJSON
    EXPECT_GT(logger.writeLog("Test CONNECTION", CONNECTION_LOG), 0);

    // C API wrappers
    EXPECT_GT(audit_log_write("C API Test", INFO_LOG), 0);
    EXPECT_GT(audit_log_write_fmt(INFO_LOG, "C API %s", "Formatted"), 0);

    unlink("/tmp/test_system.log");
    unlink(REGISTER_CONNECTIONS_LOG);
}

// The structured JSON log line is built by string concatenation, not by a JSON library, so
// every character that has meaning inside a JSON string has to be escaped by hand. Until now
// the only message ever logged through this path in tests was plain text, so none of that
// escaping was exercised: a single quote in a log message would have produced a line no log
// collector could parse, and the tests would still have been green.
TEST(EventAuditLoggerTest, JsonLogLineEscapesCharactersThatWouldBreakTheEnvelope)
{
    EventAuditLogger logger;
    logger.setLogFile("/tmp/test_escape.log");

    const std::string raw = "quote:\" backslash:\\ newline:\n tab:\t cr:\r ctrl:\x01 end";

    testing::internal::CaptureStdout();
    EXPECT_GT(logger.writeLog(raw.c_str(), CONNECTION_LOG), 0);
    const std::string captured = testing::internal::GetCapturedStdout();

    unlink("/tmp/test_escape.log");
    unlink(REGISTER_CONNECTIONS_LOG);

    ASSERT_FALSE(captured.empty());

    // Parsing is the assertion: if any of the escapes were missing the line would not be
    // valid JSON at all.
    const auto entry = nlohmann::json::parse(captured);

    EXPECT_EQ(entry["level"], "CONNECTION");
    ASSERT_TRUE(entry.contains("message"));

    // Control characters other than the ones with a short escape are dropped rather than
    // emitted raw, so the round-trip loses exactly the 0x01 and nothing else.
    const std::string expected = "quote:\" backslash:\\ newline:\n tab:\t cr:\r ctrl: end";
    EXPECT_EQ(entry["message"].get<std::string>(), expected);
}

// ADR-019 lists "service" in the log schema and Loki filters by it (US-306 AC1). The name is read
// once from OTEL_SERVICE_NAME; the unit tests do not set it, so the default must appear.
TEST(EventAuditLoggerTest, JsonLogLineCarriesTheServiceName)
{
    EventAuditLogger logger;
    logger.setLogFile("/tmp/test_service.log");

    testing::internal::CaptureStdout();
    EXPECT_GT(logger.writeLog("service field", INFO_LOG), 0);
    const std::string captured = testing::internal::GetCapturedStdout();

    unlink("/tmp/test_service.log");
    unlink(REGISTER_CONNECTIONS_LOG);

    ASSERT_FALSE(captured.empty());
    const auto entry = nlohmann::json::parse(captured);
    EXPECT_EQ(entry["service"], DEFAULT_SERVICE_NAME);
}

// Logging must never take the caller down because the log destination is unavailable: a full
// disk, a read-only mount or a bad configured path all surface here as open() failing.
TEST(EventAuditLoggerTest, ReturnsZeroWhenTheLogFileCannotBeOpened)
{
    EventAuditLogger logger;
    logger.setLogFile("/proc/self/no/such/directory/eop.log");

    EXPECT_EQ(logger.writeLog("message that cannot be persisted", INFO_LOG), 0);
}

// The observability NFR requires every structured log line to carry trace_id/span_id so a log
// can be joined to its span. The logger reads them off the active OpenTelemetry span, and the
// default global provider hands out invalid contexts -- so without a real provider installed
// this correlation silently emits empty ids, which is exactly the failure that would go
// unnoticed in production.
TEST(EventAuditLoggerTest, JsonLogLineCarriesTheActiveSpanTraceAndSpanIds)
{
    // Restored at the end rather than cleared: leaving the global provider null makes the very
    // next test that touches tracing dereference it and crash.
    auto previousProvider = opentelemetry::trace::Provider::GetTracerProvider();

    auto exporter = std::make_unique<opentelemetry::exporter::memory::InMemorySpanExporter>();
    auto processor = opentelemetry::sdk::trace::SimpleSpanProcessorFactory::Create(std::move(exporter));
    auto provider = opentelemetry::sdk::trace::TracerProviderFactory::Create(std::move(processor));
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::TracerProvider> sharedProvider(provider.release());
    opentelemetry::trace::Provider::SetTracerProvider(sharedProvider);

    EventAuditLogger logger;
    logger.setLogFile("/tmp/test_trace_ids.log");

    std::string captured;
    std::string expectedTraceId;
    std::string expectedSpanId;

    {
        auto tracer = sharedProvider->GetTracer("eop-unit-tests");
        auto span = tracer->StartSpan("audit-log-correlation");
        opentelemetry::trace::Scope scope(span);

        char traceIdHex[33] = {0};
        char spanIdHex[17] = {0};
        const auto context = span->GetContext();
        ASSERT_TRUE(context.IsValid());
        context.trace_id().ToLowerBase16(opentelemetry::nostd::span<char, 32>(traceIdHex, 32));
        context.span_id().ToLowerBase16(opentelemetry::nostd::span<char, 16>(spanIdHex, 16));
        expectedTraceId.assign(traceIdHex, 32);
        expectedSpanId.assign(spanIdHex, 16);

        testing::internal::CaptureStdout();
        EXPECT_GT(logger.writeLog("correlated connection event", CONNECTION_LOG), 0);
        captured = testing::internal::GetCapturedStdout();

        span->End();
    }

    opentelemetry::trace::Provider::SetTracerProvider(previousProvider);

    unlink("/tmp/test_trace_ids.log");
    unlink(REGISTER_CONNECTIONS_LOG);

    ASSERT_FALSE(captured.empty());
    const auto entry = nlohmann::json::parse(captured);

    EXPECT_EQ(entry["trace_id"].get<std::string>(), expectedTraceId);
    EXPECT_EQ(entry["span_id"].get<std::string>(), expectedSpanId);
}
