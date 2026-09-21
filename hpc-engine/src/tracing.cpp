#include "hpc/tracing.hpp"

#include <cstdlib>
#include <cstring>
#include <string>

#include <opentelemetry/exporters/otlp/otlp_http_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_http_exporter_options.h>
#include <opentelemetry/sdk/resource/resource.h>
#include <opentelemetry/sdk/trace/batch_span_processor_factory.h>
#include <opentelemetry/sdk/trace/batch_span_processor_options.h>
#include <opentelemetry/sdk/trace/tracer_provider.h>
#include <opentelemetry/sdk/trace/tracer_provider_factory.h>
#include <opentelemetry/trace/provider.h>
#include <opentelemetry/trace/span_context.h>

namespace hpc
{

    namespace
    {
        constexpr const char* kTracerName = "hpc-engine";
        constexpr const char* kServiceName = "hpc-engine";
        constexpr const char* kDefaultOtlpEndpoint = "http://localhost:4318/v1/traces";

        /// A W3C traceparent is `00-<32 hex trace-id>-<16 hex span-id>-<2 hex flags>`.
        constexpr size_t kTraceparentLength = 55;
        constexpr size_t kTraceIdHexLength = 32;
        constexpr size_t kSpanIdHexLength = 16;
        constexpr size_t kTraceIdOffset = 3;
        constexpr size_t kSpanIdOffset = 36;
        constexpr size_t kFlagsOffset = 53;

        bool isLowerHex(const char* text, size_t length)
        {
            for (size_t i = 0; i < length; ++i)
            {
                const char c = text[i];
                const bool digit = (c >= '0' && c <= '9');
                const bool lower = (c >= 'a' && c <= 'f');
                if (!digit && !lower)
                {
                    return false;
                }
            }
            return true;
        }

        uint8_t hexPairToByte(const char* pair)
        {
            auto value = [](char c) -> uint8_t
            {
                if (c >= '0' && c <= '9')
                {
                    return static_cast<uint8_t>(c - '0');
                }
                return static_cast<uint8_t>(c - 'a' + 10);
            };
            return static_cast<uint8_t>((value(pair[0]) << 4) | value(pair[1]));
        }

        /**
         * Parses a W3C traceparent into a remote SpanContext.
         *
         * Returns an invalid SpanContext when the value is absent or does not match the format,
         * which callers treat as "start a new trace" rather than as an error: a caller that omits
         * or garbles the header should not cost us the engine's own span.
         */
        opentelemetry::trace::SpanContext parseTraceparent(const std::string& traceparent)
        {
            const auto invalid = opentelemetry::trace::SpanContext::GetInvalid();

            if (traceparent.size() != kTraceparentLength)
            {
                return invalid;
            }
            if (traceparent[2] != '-' || traceparent[35] != '-' || traceparent[52] != '-')
            {
                return invalid;
            }

            const char* raw = traceparent.c_str();
            if (!isLowerHex(raw, 2) || !isLowerHex(raw + kTraceIdOffset, kTraceIdHexLength) ||
                !isLowerHex(raw + kSpanIdOffset, kSpanIdHexLength) || !isLowerHex(raw + kFlagsOffset, 2))
            {
                return invalid;
            }

            uint8_t traceIdBytes[16] = {0};
            for (size_t i = 0; i < sizeof(traceIdBytes); ++i)
            {
                traceIdBytes[i] = hexPairToByte(raw + kTraceIdOffset + (i * 2));
            }

            uint8_t spanIdBytes[8] = {0};
            for (size_t i = 0; i < sizeof(spanIdBytes); ++i)
            {
                spanIdBytes[i] = hexPairToByte(raw + kSpanIdOffset + (i * 2));
            }

            const opentelemetry::trace::TraceId traceId {
                opentelemetry::nostd::span<const uint8_t, 16>(traceIdBytes, sizeof(traceIdBytes))};
            const opentelemetry::trace::SpanId spanId {
                opentelemetry::nostd::span<const uint8_t, 8>(spanIdBytes, sizeof(spanIdBytes))};

            if (!traceId.IsValid() || !spanId.IsValid())
            {
                return invalid;
            }

            const opentelemetry::trace::TraceFlags flags {hexPairToByte(raw + kFlagsOffset)};

            // is_remote = true: the parent lives in eop-server, not in this process.
            return opentelemetry::trace::SpanContext(traceId, spanId, flags, /*is_remote=*/true);
        }

        opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> getTracer()
        {
            return opentelemetry::trace::Provider::GetTracerProvider()->GetTracer(kTracerName);
        }
    } // namespace

    void InitHpcTracer()
    {
        if (const char* env = std::getenv("EOP_TRACING_ENABLED"))
        {
            if (std::strcmp(env, "0") == 0)
            {
                return;
            }
        }

        opentelemetry::exporter::otlp::OtlpHttpExporterOptions opts;
        if (const char* env = std::getenv("OTEL_EXPORTER_OTLP_ENDPOINT"))
        {
            opts.url = env;
        }
        else
        {
            opts.url = kDefaultOtlpEndpoint;
        }

        auto exporter = opentelemetry::exporter::otlp::OtlpHttpExporterFactory::Create(opts);
        opentelemetry::sdk::trace::BatchSpanProcessorOptions bspOpts;
        auto processor = opentelemetry::sdk::trace::BatchSpanProcessorFactory::Create(std::move(exporter), bspOpts);

        auto resource = opentelemetry::sdk::resource::Resource::Create({{"service.name", kServiceName}});

        auto provider = opentelemetry::sdk::trace::TracerProviderFactory::Create(std::move(processor), resource);
        opentelemetry::nostd::shared_ptr<opentelemetry::trace::TracerProvider> sharedProvider(provider.release());
        opentelemetry::trace::Provider::SetTracerProvider(sharedProvider);
    }

    void ShutdownHpcTracer()
    {
        auto provider = opentelemetry::trace::Provider::GetTracerProvider();
        auto* sdkProvider = dynamic_cast<opentelemetry::sdk::trace::TracerProvider*>(provider.get());
        if (sdkProvider != nullptr)
        {
            sdkProvider->ForceFlush();
            sdkProvider->Shutdown();
        }
        opentelemetry::trace::Provider::SetTracerProvider(
            opentelemetry::nostd::shared_ptr<opentelemetry::trace::TracerProvider>());
    }

    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> StartAnalyzeGraphSpan(const std::string& traceparent,
                                                                                       const std::string& request_id,
                                                                                       int graph_nodes,
                                                                                       int graph_edges)
    {
        opentelemetry::trace::StartSpanOptions options;
        options.kind = opentelemetry::trace::SpanKind::kServer;

        const auto parentContext = parseTraceparent(traceparent);
        if (parentContext.IsValid())
        {
            options.parent = parentContext;
        }

        auto span = getTracer()->StartSpan("hpc.analyze_graph", options);
        span->SetAttribute("operation.name", "hpc.analyze_graph");
        span->SetAttribute("request.id", request_id);
        span->SetAttribute("graph.nodes", graph_nodes);
        span->SetAttribute("graph.edges", graph_edges);
        return span;
    }

    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span>
    StartAlgorithmSpan(const opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span>& parent,
                       const std::string& algorithm)
    {
        opentelemetry::trace::StartSpanOptions options;
        options.kind = opentelemetry::trace::SpanKind::kInternal;
        if (parent)
        {
            options.parent = parent->GetContext();
        }

        auto span = getTracer()->StartSpan("hpc.algorithm." + algorithm, options);
        span->SetAttribute("algorithm.name", algorithm);
        return span;
    }

} // namespace hpc
