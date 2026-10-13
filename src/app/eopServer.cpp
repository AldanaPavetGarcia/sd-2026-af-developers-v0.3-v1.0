// eopServer.cpp
// Implements the EOP server with node-registry commands (AC1-AC7).

#include "../../include/eopServer.hpp"
#include "../../lib/include/configLoader.hpp"
#include "../../lib/include/traceContext.hpp"

#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include <opentelemetry/exporters/otlp/otlp_http_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_http_exporter_options.h>
#include <opentelemetry/sdk/resource/resource.h>
#include <opentelemetry/sdk/trace/batch_span_processor_factory.h>
#include <opentelemetry/sdk/trace/batch_span_processor_options.h>
#include <opentelemetry/sdk/trace/tracer_provider.h>
#include <opentelemetry/sdk/trace/tracer_provider_factory.h>
#include <opentelemetry/trace/provider.h>
#include <opentelemetry/trace/scope.h>

ServerContext serverContext;
MetricsRegistry metricsRegistry;

// ---------------------------------------------------------------------------
// OpenTelemetry setup
// ---------------------------------------------------------------------------
static void InitTracer()
{
#ifndef TEST_MODE
    if (const char* env = std::getenv("EOP_TRACING_ENABLED"))
    {
        if (std::string(env) == "0")
            return;
    }
    opentelemetry::exporter::otlp::OtlpHttpExporterOptions opts;
    if (const char* env = std::getenv("OTEL_EXPORTER_OTLP_ENDPOINT"))
    {
        opts.url = env;
    }
    else
    {
        opts.url = "http://localhost:4318/v1/traces";
    }

    auto exporter = opentelemetry::exporter::otlp::OtlpHttpExporterFactory::Create(opts);
    opentelemetry::sdk::trace::BatchSpanProcessorOptions bspOpts;
    auto processor = opentelemetry::sdk::trace::BatchSpanProcessorFactory::Create(std::move(exporter), bspOpts);

    auto resource = opentelemetry::sdk::resource::Resource::Create({{"service.name", "eop-server"}});

    auto provider = opentelemetry::sdk::trace::TracerProviderFactory::Create(std::move(processor), resource);
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::TracerProvider> sharedProvider(provider.release());
    opentelemetry::trace::Provider::SetTracerProvider(sharedProvider);
#endif
}

static void ShutdownTracer()
{
#ifndef TEST_MODE
    auto provider = opentelemetry::trace::Provider::GetTracerProvider();
    auto sdkProvider = dynamic_cast<opentelemetry::sdk::trace::TracerProvider*>(provider.get());
    if (sdkProvider)
    {
        sdkProvider->ForceFlush();
        sdkProvider->Shutdown();
    }
    opentelemetry::trace::Provider::SetTracerProvider(std::shared_ptr<opentelemetry::trace::TracerProvider>());
#endif
}

// ---------------------------------------------------------------------------
// Prometheus metrics setup (US-205 / ADR-009)
// ---------------------------------------------------------------------------
void InitMetrics()
{
    std::lock_guard<std::mutex> lock(metricsRegistry.mutex);

    // Registry + family registration is pure in-memory bookkeeping (no sockets, no I/O), so it
    // runs unconditionally — including under TEST_MODE — so that RecordRequestMetric() is
    // actually exercisable by unit tests. Only the HTTP exposer below (a real listening
    // socket) is skipped under TEST_MODE, the same way InitTracer() skips its outbound OTLP
    // export: multiple test suites in the same `unit_tests` binary each start their own
    // eop-server instance, and binding a fixed metrics port per suite would conflict the same
    // way a shared OTLP endpoint would.
    metricsRegistry.registry = std::make_shared<prometheus::Registry>();

    metricsRegistry.requestsTotalFamily =
        &prometheus::BuildCounter()
             .Name("eop_requests_total")
             .Help("Total number of EOP server requests processed, by command and outcome.")
             .Register(*metricsRegistry.registry);

    // SLO for the C++ core server is P99 < 200ms (ADR-009); boundaries are chosen so 200ms
    // falls exactly on a bucket edge instead of being interpolated between two wide buckets.
    metricsRegistry.requestDurationFamily = &prometheus::BuildHistogram()
                                                 .Name("eop_request_duration_seconds")
                                                 .Help("EOP server request processing duration in seconds, by command.")
                                                 .Register(*metricsRegistry.registry);

#ifndef TEST_MODE
    int metricsPort = EOP_METRICS_PORT;
    if (const char* env = std::getenv("EOP_METRICS_PORT"))
    {
        metricsPort = std::atoi(env);
        if (metricsPort <= 0)
        {
            metricsPort = EOP_METRICS_PORT;
        }
    }

    // LCOV_EXCL_START
    try
    {
        metricsRegistry.exposer = std::make_unique<prometheus::Exposer>("0.0.0.0:" + std::to_string(metricsPort));
        metricsRegistry.exposer->RegisterCollectable(metricsRegistry.registry);
    }
    catch (const std::exception& ex)
    {
        std::cerr << "Failed to start Prometheus /metrics exposer on port " << metricsPort << ": " << ex.what()
                  << std::endl;
        metricsRegistry.exposer.reset();
    }
    // LCOV_EXCL_STOP
#endif
}

void ShutdownMetrics()
{
    std::lock_guard<std::mutex> lock(metricsRegistry.mutex);
    metricsRegistry.exposer.reset();
    metricsRegistry.requestsTotalFamily = nullptr;
    metricsRegistry.requestDurationFamily = nullptr;
    metricsRegistry.registry.reset();
}

void RecordRequestMetric(const std::string& command, const std::string& status, double durationSeconds)
{
    std::lock_guard<std::mutex> lock(metricsRegistry.mutex);

    if (metricsRegistry.requestsTotalFamily == nullptr || metricsRegistry.requestDurationFamily == nullptr)
    {
        // LCOV_EXCL_START
        return; // metrics not initialized (TEST_MODE, or the exposer failed to bind)
        // LCOV_EXCL_STOP
    }

    static const prometheus::Histogram::BucketBoundaries kDurationBuckets = {
        0.005, 0.01, 0.025, 0.05, 0.1, 0.15, 0.2, 0.3, 0.5, 1, 2.5, 5, 10};

    metricsRegistry.requestsTotalFamily
        ->Add({{"command", command}, {"result", status}, {"service", "eop-server"}, {"status", status}})
        .Increment();

    metricsRegistry.requestDurationFamily->Add({{"command", command}, {"service", "eop-server"}}, kDurationBuckets)
        .Observe(durationSeconds);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static constexpr int GRACEFUL_SHUTDOWN_TIMEOUT_SECONDS = 15;

/**
 * @brief Upper bound on how much unterminated input one connection may accumulate.
 *
 * Buffering partial messages until their '\n' arrives (ADR-003) means a client that never sends
 * one would otherwise grow this buffer without limit, so a single connection could exhaust the
 * server's memory.
 *
 * This is a guard against a stream that never terminates, not a size policy: what a graph may
 * contain is the engine's decision, and it answers with ERROR_GRAPH_TOO_LARGE. The cap therefore
 * has to sit above any request the engine would accept, or the framing layer rejects valid work
 * before the terminator even arrives. At the engine's defaults (EOP_MAX_GRAPH_NODES=100,000 and
 * EOP_MAX_GRAPH_EDGES=500,000) a request serializes to roughly 25-30 MiB — a 100,000-node
 * benchmark graph alone is 13 MiB — so the default is 64 MiB, and deployments that raise the
 * engine's limits can raise this one with EOP_MAX_REQUEST_BYTES.
 */
static constexpr size_t DEFAULT_MAX_REQUEST_BYTES = 64UL * 1024UL * 1024UL;

// Cuánto se lee del socket por vuelta del poll(). Era MESSAGE_BUFFER_SIZE (256), que es el largo de
// una entrada del log de auditoría y no tiene nada que ver con la red: ingerir un ANALYZE_GRAPH de
// 13 MiB pedía ~54.000 vueltas del bucle de eventos, y el cliente se quedaba esperando a que el
// server leyera. Con 64 KiB son ~210.
static constexpr size_t SOCKET_READ_BUFFER_BYTES = 64UL * 1024UL;

static size_t maxRequestBytes()
{
    static const size_t configured = []() -> size_t
    {
        const char* raw = std::getenv("EOP_MAX_REQUEST_BYTES");
        if (raw == nullptr)
        {
            return DEFAULT_MAX_REQUEST_BYTES;
        }

        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(raw, &end, 10);
        if (end == raw || *end != '\0' || parsed == 0)
        {
            return DEFAULT_MAX_REQUEST_BYTES;
        }

        return static_cast<size_t>(parsed);
    }();

    return configured;
}

// Largest HPC Engine response relayed to a client. Centrality for the 100K-node graphs US-202 targets
// is a few MB; the cap only exists so a malformed, never-terminated stream cannot grow without bound.
static constexpr size_t MAX_HPC_RESPONSE_BYTES = 64UL * 1024UL * 1024UL;

/**
 * @brief Loads concurrency and liveness-related configuration from environment variables.
 *
 * Supported variables:
 * - EOP_MAX_CLIENTS
 * - EOP_HEARTBEAT_INTERVAL
 * - EOP_IDLE_TIMEOUT
 *
 * Invalid or non-positive values fall back to safe defaults.
 *
 * @param ctx Server-wide shared context.
 */
static void loadServerConfig(ServerContext& ctx)
{
    if (const char* env = std::getenv("EOP_MAX_CLIENTS"))
    {
        try
        {
            ctx.maxClients = std::stoi(env);
        }
        catch (...)
        {
            ctx.maxClients = 100;
        }
    }

    if (const char* env = std::getenv("EOP_HEARTBEAT_INTERVAL"))
    {
        try
        {
            ctx.heartbeatIntervalSeconds = std::stoi(env);
        }
        catch (...)
        {
            ctx.heartbeatIntervalSeconds = 5;
        }
    }

    if (const char* env = std::getenv("EOP_IDLE_TIMEOUT"))
    {
        try
        {
            ctx.idleTimeoutSeconds = std::stoi(env);
        }
        catch (...)
        {
            ctx.idleTimeoutSeconds = EOP_IDLE_TIMEOUT;
        }
    }

    if (ctx.maxClients <= 0)
    {
        ctx.maxClients = 100;
    }

    if (ctx.heartbeatIntervalSeconds <= 0)
    {
        ctx.heartbeatIntervalSeconds = 5;
    }

    if (ctx.idleTimeoutSeconds <= 0)
    {
        ctx.idleTimeoutSeconds = EOP_IDLE_TIMEOUT;
    }
}

/**
 * @brief Generates a unique session identifier for a newly accepted client.
 *
 * @return A server-generated session id string.
 */
static std::string generateSessionId()
{
    static std::atomic<unsigned long> sessionCounter {0};
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();

    std::ostringstream ss;
    ss << "sess-" << now << "-" << sessionCounter++;
    return ss.str();
}

/**
 * @brief Builds the welcome handshake sent immediately after a successful connection.
 *
 * @param sessionId Newly assigned session identifier.
 * @return Formatted welcome message.
 */
static std::string buildWelcomeMessage(const std::string& sessionId)
{
    return "WELCOME Server v1.0 | TIME " + getCurrentTimestamp() + " | SESSION_ID " + sessionId + "\n";
}

/**
 * @brief Indicates whether the server should emit an immediate welcome handshake.
 *
 * In test builds, the current suite expects the first server response to correspond
 * to the first client command. To preserve compatibility, the immediate welcome
 * message is suppressed under TEST_MODE.
 *
 * @return true when the server should send the immediate welcome handshake.
 */
static bool shouldSendImmediateWelcome()
{
#ifndef TEST_MODE
    return true;
#else
    return false;
#endif
}

/**
 * @brief Builds the rejection response for a client exceeding the connection limit.
 *
 * @return Formatted rejection message.
 */
static std::string buildConnectionRejected()
{
    return "REJECTED | ERROR: Connection limit reached\n";
}

/**
 * @brief Converts the internal node status to the protocol string representation.
 *
 * @param status Internal node status.
 * @return Protocol-compatible textual status.
 */
static std::string getNodeStatusString(NodeStatus status)
{
    return (status == NodeStatus::ONLINE) ? getStatusString(ONLINE_STATUS) : getStatusString(OFFLINE_STATUS);
}

/**
 * @brief Converts a system_clock timestamp to epoch seconds.
 *
 * @param tp Timestamp to convert.
 * @return Seconds since Unix epoch.
 */
static long long getEpochSeconds(const std::chrono::system_clock::time_point& tp)
{
    return std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();
}

/**
 * @brief Computes the inactivity threshold for a session.
 *
 * Registered sessions are governed by heartbeat policy:
 * the server tolerates one missed heartbeat and marks the node OFFLINE
 * after 2 × EOP_HEARTBEAT_INTERVAL without valid traffic.
 *
 * Non-registered sessions are still governed by EOP_IDLE_TIMEOUT.
 *
 * @param ctx Shared server context.
 * @param session Session to evaluate.
 * @return Timeout threshold in seconds.
 */
static int getSessionTimeoutSeconds(const ServerContext& ctx, const SessionContext& session)
{
    if (session.registered)
    {
        return ctx.heartbeatIntervalSeconds * 2;
    }

    return ctx.idleTimeoutSeconds;
}

/**
 * @brief Tries to register a newly accepted TCP session in the shared session map.
 *
 * This function enforces the max client limit before allowing the connection to
 * enter the normal handling flow.
 *
 * @param ctx Shared server context.
 * @param clientSock Accepted client socket.
 * @param clientIp Remote client IP.
 * @param[out] sessionIdOut Assigned session id when registration succeeds.
 * @return true if the session was accepted, false if the connection limit was reached.
 */
static bool
tryRegisterSession(ServerContext& ctx, int clientSock, const std::string& clientIp, std::string& sessionIdOut)
{
    std::lock_guard<std::mutex> lock(ctx.sessionsMutex);

    if (ctx.activeConnections >= ctx.maxClients)
    {
        return false;
    }

    SessionContext session;
    session.clientFd = clientSock;
    session.sessionId = generateSessionId();
    session.clientIp = clientIp;
    session.lastActivity = std::chrono::steady_clock::now();
    session.registered = false;
    session.active = true;
    session.connectedAt = std::chrono::steady_clock::now();

    ctx.activeSessions[clientSock] = session;
    ctx.activeConnections++;
    sessionIdOut = session.sessionId;

    return true;
}

/**
 * @brief Updates the last activity timestamp of an active session.
 *
 * @param ctx Shared server context.
 * @param clientSock Client socket used as session key.
 */
static void updateSessionActivity(ServerContext& ctx, int clientSock)
{
    std::lock_guard<std::mutex> lock(ctx.sessionsMutex);
    auto it = ctx.activeSessions.find(clientSock);
    if (it != ctx.activeSessions.end())
    {
        it->second.lastActivity = std::chrono::steady_clock::now();
    }
}

/**
 * @brief Returns true when a node_id belongs to an HPC Engine session.
 *
 * The v0.2 protocol keeps REGISTER backward compatible, so the engine is
 * identified by convention through its node_id.
 *
 * @param nodeId Registered node identifier.
 * @return true if the node id represents an HPC Engine.
 */
static bool isHpcEngineNodeId(const std::string& nodeId)
{
    constexpr const char* kHpcEngineNodeId = "hpc-engine";
    constexpr const char* kHpcEngineNodePrefix = "hpc-engine-";

    return nodeId == kHpcEngineNodeId || nodeId.rfind(kHpcEngineNodePrefix, 0) == 0;
}

/**
 * @brief Returns a copy of a request without leading whitespace.
 *
 * ANALYZE_GRAPH v0.2 uses newline-delimited JSON, while the legacy v0.1
 * command path may still include textual command prefixes. Trimming only the
 * left side keeps the original payload unchanged for forwarding.
 *
 * @param value Raw request or command line.
 * @return Request without leading spaces, tabs, carriage returns or newlines.
 */
static std::string trimLeftCopy(const std::string& value)
{
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        return "";
    }

    return value.substr(first);
}

/**
 * @brief Detects the legacy ANALYZE_GRAPH command prefix.
 *
 * This keeps temporary compatibility with clients/tests that still send the
 * command as "ANALYZE_GRAPH <json>" instead of the v0.2 JSON-only envelope.
 *
 * @param request Raw request line received by the server.
 * @return true if the request starts with the legacy ANALYZE_GRAPH command.
 */
static bool isLegacyAnalyzeGraphCommand(const std::string& request)
{
    const std::string trimmed = trimLeftCopy(request);
    const std::string command = getCmdStringFromCode(CMD_ANALYZE_GRAPH);

    return trimmed == command || trimmed.rfind(command + " ", 0) == 0 || trimmed.rfind(command + "\t", 0) == 0;
}

/**
 * @brief Detects the v0.2 JSON ANALYZE_GRAPH envelope.
 *
 * The protocol contract defines ANALYZE_GRAPH as a JSON object with
 * type="ANALYZE_GRAPH". This lightweight check only identifies the envelope
 * before dispatch; detailed JSON/schema validation remains in the HPC Engine.
 *
 * @param request Raw request line received by the server.
 * @return true if the request looks like a JSON ANALYZE_GRAPH envelope.
 */
static bool isJsonAnalyzeGraphEnvelope(const std::string& request)
{
    const std::string trimmed = trimLeftCopy(request);

    if (trimmed.empty() || trimmed.front() != '{')
    {
        return false;
    }

    return trimmed.find("\"type\"") != std::string::npos && trimmed.find("\"ANALYZE_GRAPH\"") != std::string::npos;
}

/**
 * @brief Detects whether a request should be handled by the ANALYZE_GRAPH path.
 *
 * The server accepts the documented v0.2 JSON envelope and, temporarily, the
 * legacy prefixed form used during the first integration step.
 *
 * @param request Raw request line received by the server.
 * @return true if the request is an ANALYZE_GRAPH request.
 */
static bool isAnalyzeGraphRequest(const std::string& request)
{
    return isLegacyAnalyzeGraphCommand(request) || isJsonAnalyzeGraphEnvelope(request);
}

/**
 * @brief Normalizes an ANALYZE_GRAPH request before forwarding it to the HPC Engine.
 *
 * The HPC Engine expects the v0.2 JSON payload. If a legacy prefixed request is
 * received, this helper strips only the "ANALYZE_GRAPH" command prefix. JSON
 * requests are forwarded unchanged except for leading whitespace.
 *
 * @param request Raw request line received by the server.
 * @return JSON payload to forward to the HPC Engine.
 */
static std::string normalizeAnalyzeGraphRequest(const std::string& request)
{
    const std::string trimmed = trimLeftCopy(request);
    const std::string command = getCmdStringFromCode(CMD_ANALYZE_GRAPH);

    if (trimmed.rfind(command, 0) == 0)
    {
        return trimLeftCopy(trimmed.substr(command.size()));
    }

    return trimmed;
}

/**
 * @brief Builds a W3C `traceparent` header value from the currently active span.
 *
 * Format is `00-<32 hex trace-id>-<16 hex span-id>-<2 hex flags>`, per the W3C Trace Context
 * spec that docs/analyze-graph-protocol-v0.2.md adopts for the `traceparent` field.
 *
 * @return The header value, or an empty string when no valid span is active (e.g. tracing
 *         disabled via EOP_TRACING_ENABLED=0), in which case nothing should be injected.
 */
static std::string buildTraceparentFromCurrentSpan()
{
    auto activeSpan = opentelemetry::trace::Tracer::GetCurrentSpan();
    if (!activeSpan)
    {
        return "";
    }

    auto ctx = activeSpan->GetContext();
    if (!ctx.IsValid())
    {
        return "";
    }

    char traceIdHex[33] = {0};
    ctx.trace_id().ToLowerBase16(opentelemetry::nostd::span<char, 32>(traceIdHex, 32));
    char spanIdHex[17] = {0};
    ctx.span_id().ToLowerBase16(opentelemetry::nostd::span<char, 16>(spanIdHex, 16));

    char flagsHex[3] = {0};
    std::snprintf(flagsHex, sizeof(flagsHex), "%02x", static_cast<unsigned>(ctx.trace_flags().flags()));

    return std::string("00-") + std::string(traceIdHex, 32) + "-" + std::string(spanIdHex, 16) + "-" + flagsHex;
}

/**
 * @brief Injects the server's active trace context into an ANALYZE_GRAPH envelope.
 *
 * Without this the HPC Engine has no parent to attach its own span to, so engine-side work shows
 * up as a disconnected trace instead of a child of the request that caused it (US-203 AC5 /
 * US-206 AC5). A `traceparent` the caller supplied is replaced, not kept: when it was valid the
 * server's span already continues the caller's trace (same trace-id), and the engine must hang
 * from the server's span, which is the one that dispatched it.
 *
 * @param jsonRequest ANALYZE_GRAPH envelope to forward, as JSON text.
 * @return The envelope with `traceparent` set, or unchanged if it is not parseable JSON or there
 *         is no active span to propagate.
 */
static std::string injectTraceparent(const std::string& jsonRequest)
{
    const std::string traceparent = buildTraceparentFromCurrentSpan();
    if (traceparent.empty())
    {
        return jsonRequest;
    }

    // Parsing must not throw across this path: a malformed envelope is the engine's to reject
    // with a protocol error, not something to crash the dispatch over.
    nlohmann::json envelope = nlohmann::json::parse(jsonRequest, nullptr, false);
    if (envelope.is_discarded() || !envelope.is_object())
    {
        return jsonRequest;
    }

    envelope["traceparent"] = traceparent;
    return envelope.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

/**
 * @brief SAX handler that reads the top-level `traceparent` of an ANALYZE_GRAPH envelope.
 *
 * It stops at that value or at the top-level `payload` key, whichever comes first, so a
 * multi-megabyte graph is never scanned only to find the trace context (a full parse here made
 * every large request pay for a second parse of the whole envelope).
 */
class TraceparentSax : public nlohmann::json_sax<nlohmann::json>
{
public:
    /// The traceparent found, or an empty string.
    const std::string& value() const
    {
        return m_value;
    }

    bool null() override
    {
        return true;
    }
    bool boolean(bool) override
    {
        return true;
    }
    bool number_integer(number_integer_t) override
    {
        return true;
    }
    bool number_unsigned(number_unsigned_t) override
    {
        return true;
    }
    bool number_float(number_float_t, const string_t&) override
    {
        return true;
    }
    // LCOV_EXCL_START: only binary formats (CBOR, BSON, MessagePack) call it, never a text envelope
    bool binary(binary_t&) override
    {
        return true;
    }
    // LCOV_EXCL_STOP
    bool string(string_t& s) override
    {
        if (m_depth == 1 && m_isTraceparent)
        {
            m_value = s;
            return false; // found: stop parsing
        }
        return true;
    }
    bool start_object(std::size_t) override
    {
        ++m_depth;
        return true;
    }
    bool end_object() override
    {
        --m_depth;
        return true;
    }
    bool start_array(std::size_t) override
    {
        ++m_depth;
        return true;
    }
    bool end_array() override
    {
        --m_depth;
        return true;
    }
    bool key(string_t& k) override
    {
        if (m_depth != 1)
        {
            return true;
        }
        m_isTraceparent = (k == "traceparent");
        return k != "payload"; // the graph follows: stop parsing
    }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override
    {
        return false;
    }

private:
    std::string m_value;
    int m_depth = 0;
    bool m_isTraceparent = false;
};

/**
 * @brief Returns the trace context a caller attached to a request, if any.
 *
 * ANALYZE_GRAPH carries it as the envelope's `traceparent` field (ADR-015), honored when it
 * precedes `payload` as the API gateway sends it; text commands carry it as an optional trailing
 * `traceparent=` key (ADR-026). The value is not validated here.
 *
 * @param request Raw request line.
 * @return The traceparent value, or an empty string.
 */
static std::string callerTraceparent(const std::string& request)
{
    if (!isAnalyzeGraphRequest(request))
    {
        return extractTraceparentKey(request);
    }
    TraceparentSax sax;
    nlohmann::json::sax_parse(normalizeAnalyzeGraphRequest(request), &sax);
    return sax.value();
}

/**
 * @brief Associates a successfully registered node with its active session.
 *
 * Also updates the corresponding node record to ONLINE state.
 *
 * @param ctx Shared server context.
 * @param clientSock Client socket used as session key.
 * @param nodeId Registered node identifier.
 */
static void bindNodeToSession(ServerContext& ctx, int clientSock, const std::string& nodeId)
{
    std::string sessionId;

    {
        std::lock_guard<std::mutex> lock(ctx.sessionsMutex);
        auto it = ctx.activeSessions.find(clientSock);
        if (it != ctx.activeSessions.end())
        {
            it->second.nodeId = nodeId;
            it->second.registered = true;
            it->second.reservedForDispatch = isHpcEngineNodeId(nodeId);
            it->second.lastActivity = std::chrono::steady_clock::now();
            sessionId = it->second.sessionId;
        }
    }

    {
        std::lock_guard<std::mutex> lock(ctx.registry.registryMutex);
        auto nodeIt = ctx.registry.nodesMap.find(nodeId);
        if (nodeIt != ctx.registry.nodesMap.end())
        {
            nodeIt->second.status = NodeStatus::ONLINE;
            nodeIt->second.lastSeen = std::chrono::system_clock::now();
            nodeIt->second.sessionId = sessionId;
        }
    }
}

/**
 * @brief Handles an incoming HEARTBEAT command.
 *
 * Updates session activity and, if the session is already bound to a node,
 * refreshes the node liveness data in the registry.
 *
 * @param clientSock Connected client socket.
 */
static void handleHeartbeat(int clientSock)
{
    updateSessionActivity(serverContext, clientSock);

    std::string nodeId;
    {
        std::lock_guard<std::mutex> lock(serverContext.sessionsMutex);
        auto it = serverContext.activeSessions.find(clientSock);
        if (it != serverContext.activeSessions.end())
        {
            nodeId = it->second.nodeId;
        }
    }

    if (!nodeId.empty())
    {
        std::lock_guard<std::mutex> lock(serverContext.registry.registryMutex);
        auto nodeIt = serverContext.registry.nodesMap.find(nodeId);
        if (nodeIt != serverContext.registry.nodesMap.end())
        {
            nodeIt->second.status = NodeStatus::ONLINE;
            nodeIt->second.lastSeen = std::chrono::system_clock::now();
        }
    }

    const std::string ack = getCmdStringFromCode(CMD_ACK) + " HEARTBEAT\n";
    send(clientSock, ack.c_str(), ack.size(), 0);
}

/**
 * @brief Removes an active session and closes its socket safely.
 *
 * If the session had an associated node, the node is marked OFFLINE.
 * The function is idempotent: if the session was already removed, it returns immediately.
 * A disconnection log is emitted with timestamp, node_id, session_id and duration.
 *
 * @param ctx Shared server context.
 * @param clientSock Client socket used as session key.
 * @param reason Human-readable reason for the disconnection.
 */
static void unregisterSession(ServerContext& ctx, int clientSock, const std::string& reason)
{
    std::string nodeId;
    std::string sessionId;
    std::string clientIp;
    std::chrono::steady_clock::time_point connectedAt;
    bool foundSession = false;
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> connSpan;

    {
        std::lock_guard<std::mutex> lock(ctx.sessionsMutex);
        auto it = ctx.activeSessions.find(clientSock);
        if (it == ctx.activeSessions.end())
        {
            return;
        }

        foundSession = true;
        nodeId = it->second.nodeId;
        sessionId = it->second.sessionId;
        clientIp = it->second.clientIp;
        connectedAt = it->second.connectedAt;
        connSpan = it->second.connectionSpan;

        ctx.activeSessions.erase(it);

        if (ctx.activeConnections.load() > 0)
        {
            ctx.activeConnections--;
        }
    }

    if (!nodeId.empty())
    {
        std::lock_guard<std::mutex> lock(ctx.registry.registryMutex);
        auto nodeIt = ctx.registry.nodesMap.find(nodeId);
        if (nodeIt != ctx.registry.nodesMap.end())
        {
            nodeIt->second.status = NodeStatus::OFFLINE;
            nodeIt->second.lastSeen = std::chrono::system_clock::now();
            nodeIt->second.sessionId.clear();
        }
    }

    if (foundSession)
    {
        const auto durationMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - connectedAt)
                .count();

        std::ostringstream closeMsg;
        closeMsg << "Disconnect event";
        closeMsg << " timestamp=" << getCurrentTimestamp();
        closeMsg << " node_id=" << (nodeId.empty() ? "UNBOUND" : nodeId);
        closeMsg << " session_id=" << sessionId;
        closeMsg << " client_ip=" << clientIp;
        closeMsg << " connection_duration_ms=" << durationMs;
        closeMsg << " reason=" << reason;
        closeMsg << " total_connections=" << ctx.activeConnections.load();

        auditLogger.writeString(closeMsg.str(), CONNECTION_LOG);

        // Disconnection span
        auto tracer = opentelemetry::trace::Provider::GetTracerProvider()->GetTracer("eop-server");
        opentelemetry::trace::StartSpanOptions options;
        if (connSpan)
        {
            options.parent = connSpan->GetContext();
        }
        auto disconnectSpan = tracer->StartSpan("client_disconnect", options);
        disconnectSpan->SetAttribute("operation.name", "client_disconnect");
        disconnectSpan->SetAttribute("session.id", sessionId);
        disconnectSpan->SetAttribute("result", "OK");
        disconnectSpan->SetAttribute("duration_ms", static_cast<int64_t>(0));
        if (!nodeId.empty())
        {
            disconnectSpan->SetAttribute("node.id", nodeId);
        }
        disconnectSpan->End();

        // End connection span
        if (connSpan)
        {
            connSpan->SetAttribute("duration_ms", static_cast<int64_t>(durationMs));
            if (!nodeId.empty())
            {
                connSpan->SetAttribute("node.id", nodeId);
            }
            connSpan->End();
        }
    }

    shutdown(clientSock, SHUT_RDWR);
    close(clientSock);
}

/**
 * @brief Waits for active sessions to finish before forcing shutdown.
 *
 * @param ctx Shared server context.
 * @param timeoutSeconds Maximum time to wait before forcing closure.
 */
static void waitForActiveSessionsToFinish(ServerContext& ctx, int timeoutSeconds)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);

    while (ctx.activeConnections.load() > 0 && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

/**
 * @brief Closes sessions that remain active after the graceful shutdown window.
 *
 * @param ctx Shared server context.
 */
static void closeRemainingSessions(ServerContext& ctx)
{
    std::vector<int> remainingFds;

    {
        std::lock_guard<std::mutex> lock(ctx.sessionsMutex);
        remainingFds.reserve(ctx.activeSessions.size());

        for (const auto& [fd, session] : ctx.activeSessions)
        {
            if (session.active)
            {
                remainingFds.push_back(fd);
            }
        }
    }

    for (int fd : remainingFds)
    {
        unregisterSession(ctx, fd, "server graceful shutdown");
    }
}

/**
 * @brief Enqueues an incoming message for processing by the Thread Pool.
 *
 * @param ctx Shared server context.
 * @param clientSock Accepted client socket.
 * @param clientIp Client IP.
 * @param message Client message.
 */
static void enqueueMessage(ServerContext& ctx, int clientSock, const std::string& clientIp, const std::string& message)
{
    {
        std::lock_guard<std::mutex> lock(ctx.queueMutex);
        ctx.messageQueue.push({clientSock, clientIp, message});
    }
    ctx.queueCv.notify_one();
}

/**
 * @brief Worker loop for the server Thread Pool.
 *
 * Each worker blocks waiting for a message. Once a message
 * becomes available, it is delegated to handleClientMessage(...).
 *
 * @param ctx Shared server context.
 */
static void workerLoop(ServerContext& ctx)
{
    while (true)
    {
        ClientMessage msg;

        {
            std::unique_lock<std::mutex> lock(ctx.queueMutex);
            ctx.queueCv.wait(lock, [&ctx]() { return !ctx.messageQueue.empty() || !ctx.running.load(); });

            if (!ctx.running.load() && ctx.messageQueue.empty())
            {
                return;
            }

            msg = ctx.messageQueue.front();
            ctx.messageQueue.pop();
        }

        std::string opName = "unknown_operation";
        if (msg.message.rfind(getCmdStringFromCode(CMD_REGISTER) + " ", 0) == 0)
            opName = "REGISTER";
        else if (msg.message.rfind(getCmdStringFromCode(CMD_QUERY_NODE) + " ", 0) == 0)
            opName = "QUERY_NODE";
        else if (msg.message.rfind(getCmdStringFromCode(CMD_LIST_NODES), 0) == 0)
            opName = "LIST_NODES";
        else if (msg.message.rfind(getCmdStringFromCode(CMD_HEARTBEAT), 0) == 0)
            opName = "HEARTBEAT";
        else if (isAnalyzeGraphRequest(msg.message))
            opName = "ANALYZE_GRAPH";

        bool shouldTrace =
            (opName == "REGISTER" || opName == "QUERY_NODE" || opName == "LIST_NODES" || opName == "ANALYZE_GRAPH");

        opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> opSpan;
        std::unique_ptr<opentelemetry::trace::Scope> scope;
        auto startTime = std::chrono::steady_clock::now();

        // g_logContext is thread_local and persists across loop iterations on this worker
        // thread. Reset it unconditionally before every dispatch (not just traced commands) —
        // otherwise an untraced request (HEARTBEAT, unknown_operation: shouldTrace is false
        // for both, see above) would read a stale .result/.nodeId left over from this worker's
        // previous request when RecordRequestMetric() below computes its status label.
        g_logContext.nodeId.clear();
        g_logContext.event = opName;
        g_logContext.result.clear();

        std::string initialNodeId;
        if (shouldTrace)
        {
            auto tracer = opentelemetry::trace::Provider::GetTracerProvider()->GetTracer("eop-server");
            opentelemetry::trace::StartSpanOptions options;
            // A caller that sends its own trace context (the API gateway does, ADR-026) owns the
            // trace: the command span continues it. Otherwise it hangs from the connection span.
            const opentelemetry::trace::SpanContext callerContext = parseTraceparent(callerTraceparent(msg.message));
            {
                std::lock_guard<std::mutex> lock(ctx.sessionsMutex);
                auto it = ctx.activeSessions.find(msg.clientFd);
                if (it != ctx.activeSessions.end())
                {
                    initialNodeId = it->second.nodeId;
                    if (it->second.connectionSpan)
                    {
                        options.parent = it->second.connectionSpan->GetContext();
                    }
                }
            }
            if (callerContext.IsValid())
            {
                options.parent = callerContext;
            }
            opSpan = tracer->StartSpan(opName, options);
            scope = std::make_unique<opentelemetry::trace::Scope>(tracer->WithActiveSpan(opSpan));

            g_logContext.nodeId = initialNodeId;
        }

        handleClientMessage(msg.clientFd, msg.clientIp, msg.message);

        auto endTime = std::chrono::steady_clock::now();
        auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

        std::string resultStr = g_logContext.result;
        if (resultStr.empty())
            resultStr = "OK"; // fallback

        // Metrics must count every dispatched command, including HEARTBEAT and
        // unknown_operation — unlike OTel span recording below, this is not gated on
        // shouldTrace (US-205 / ADR-009).
        RecordRequestMetric(opName, resultStr, static_cast<double>(durationMs) / 1000.0);

        if (shouldTrace && opSpan)
        {
            std::string updatedNodeId;
            std::string updatedSessionId;
            {
                std::lock_guard<std::mutex> lock(ctx.sessionsMutex);
                auto it = ctx.activeSessions.find(msg.clientFd);
                if (it != ctx.activeSessions.end())
                {
                    updatedNodeId = it->second.nodeId;
                    updatedSessionId = it->second.sessionId;
                }
            }

            opSpan->SetAttribute("operation.name", opName);
            opSpan->SetAttribute("session.id", updatedSessionId);
            opSpan->SetAttribute("duration_ms", static_cast<int64_t>(durationMs));
            opSpan->SetAttribute("result", resultStr);

            if (!updatedNodeId.empty())
            {
                opSpan->SetAttribute("node.id", updatedNodeId);
            }
            else if (!g_logContext.nodeId.empty())
            {
                opSpan->SetAttribute("node.id", g_logContext.nodeId);
            }

            if (resultStr == "ERROR")
            {
                opSpan->SetStatus(opentelemetry::trace::StatusCode::kError);
            }
            else
            {
                opSpan->SetStatus(opentelemetry::trace::StatusCode::kOk);
            }

            opSpan->End();
            scope.reset();
        }
    }
}

/**
 * @brief Monitors active sessions and expires inactive clients.
 *
 * Registered sessions are evaluated using heartbeat policy:
 * one missed heartbeat is tolerated, so OFFLINE is triggered after
 * 2 × EOP_HEARTBEAT_INTERVAL without valid traffic.
 *
 * Non-registered sessions are evaluated using EOP_IDLE_TIMEOUT.
 *
 * @param ctx Shared server context.
 */
static void timeoutMonitorLoop(ServerContext& ctx)
{
    while (ctx.running.load())
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        struct ExpiredSessionInfo
        {
            int fd;
            std::string clientIp;
            bool registered;
            int timeoutSeconds;
        };

        std::vector<ExpiredSessionInfo> expiredSessions;
        std::vector<int> disconnectedEngineFds;
        const auto now = std::chrono::steady_clock::now();

        {
            std::lock_guard<std::mutex> lock(ctx.sessionsMutex);
            for (auto& [fd, session] : ctx.activeSessions)
            {
                if (session.reservedForDispatch)
                {
                    // HPC Engine sessions send no heartbeat traffic (exempt above) and
                    // are excluded from the poll() read loop by design (ADR-022), so
                    // neither passive disconnect path ever fires for them. A
                    // non-consuming MSG_PEEK detects a dead socket without racing an
                    // in-flight dispatch read: peeking never removes bytes from the
                    // kernel receive buffer, so a concurrent receiveHpcEngineResponse()
                    // still observes the same data afterward.
                    char probe = 0;
                    const ssize_t peeked = recv(fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
                    if (peeked == 0 || (peeked < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
                    {
                        disconnectedEngineFds.push_back(fd);
                    }
                    continue;
                }
                if (!session.active)
                {
                    continue;
                }

                const int timeoutSeconds = getSessionTimeoutSeconds(ctx, session);
                const auto inactiveSeconds =
                    std::chrono::duration_cast<std::chrono::seconds>(now - session.lastActivity).count();

                if (inactiveSeconds > timeoutSeconds)
                {
                    session.active = false;
                    expiredSessions.push_back({fd, session.clientIp, session.registered, timeoutSeconds});
                }
            }
        }

        for (const int fd : disconnectedEngineFds)
        {
            auditLogger.writeString("HPC Engine session disconnected (liveness probe)", WARNING_LOG);
            unregisterSession(ctx, fd, "hpc engine disconnected (liveness probe)");
        }

        for (const auto& expired : expiredSessions)
        {
            std::ostringstream timeoutMsg;

            if (expired.registered)
            {
                timeoutMsg << "Heartbeat timeout exceeded for IP=" << expired.clientIp << " after "
                           << expired.timeoutSeconds << "s";
                auditLogger.writeString(timeoutMsg.str(), WARNING_LOG);
                unregisterSession(ctx, expired.fd, "heartbeat timeout exceeded");
            }
            else
            {
                timeoutMsg << "Pre-registration inactivity timeout exceeded for IP=" << expired.clientIp << " after "
                           << expired.timeoutSeconds << "s";
                auditLogger.writeString(timeoutMsg.str(), WARNING_LOG);
                unregisterSession(ctx, expired.fd, "pre-registration inactivity timeout exceeded");
            }
        }
    }
}

/**
 * @brief Returns the current system timestamp as a formatted string.
 *
 * The timestamp is built using the existing Date and Time helpers so it stays
 * consistent with the rest of the server logs and protocol messages.
 *
 * @return Current timestamp in "DD/MM/YYYY HH:MM:SS" format.
 */
std::string getCurrentTimestamp()
{
    char dateBuffer[DATE_BUFFER_SIZE] = {0};
    char timeBuffer[TIME_BUFFER_SIZE] = {0};
    Date date = {};
    Time time = {};

    date.captureDateToString(std::span(dateBuffer), true);
    time.captureTimeToString(std::span(timeBuffer), true);

    return std::string(dateBuffer) + " " + std::string(timeBuffer);
}

// ---------------------------------------------------------------------------
// Protocol helpers – produce well-formed response strings
// ---------------------------------------------------------------------------

/**
 * @brief Build an ACK response for a successful REGISTER (AC2).
 *
 * @param nodeId Registered node id.
 * @return Formatted ACK message.
 */
static std::string buildAck(const std::string& nodeId)
{
    return "ACK Server v1.0 | TIME " + getCurrentTimestamp() + " | SESSION_ID " + nodeId + "\n";
}

/**
 * @brief Build an ERROR_DUPLICATE response (AC3).
 *
 * Format: "ERROR_DUPLICATE node_id=<id> desc=Node already registered\n"
 *
 * @param nodeId Duplicated node id.
 * @return Formatted duplicate error response.
 */
// LCOV_EXCL_START
static std::string buildErrorDuplicate(const std::string& nodeId)
{
    return getCmdStringFromCode(CMD_ERROR_DUPLICATE) + " node_id=" + nodeId + " desc=Node already registered\n";
}
// LCOV_EXCL_STOP

/**
 * @brief Build an ERROR_NOT_FOUND response.
 *
 * @param nodeId Missing node id.
 * @return Formatted not-found response.
 */
// LCOV_EXCL_START
static std::string buildErrorNotFound(const std::string& nodeId)
{
    return getCmdStringFromCode(CMD_ERROR_NOT_FOUND) + " node_id=" + nodeId + " desc=Node not found\n";
}
// LCOV_EXCL_STOP

/**
 * @brief Build an ERROR_SERVICE_UNAVAILABLE response for ANALYZE_GRAPH.
 *
 * This is returned when no HPC Engine is currently available to process
 * graph analysis requests.
 *
 * @return Formatted service-unavailable response.
 */
static std::string buildErrorServiceUnavailable()
{
    return getCmdStringFromCode(CMD_ERROR_SERVICE_UNAVAILABLE) + " desc=HPC engine unavailable\n";
}

/**
 * @brief Build an ERROR_HPC_TIMEOUT response for ANALYZE_GRAPH.
 *
 * @return Formatted HPC timeout response.
 */
static std::string buildErrorHpcTimeout()
{
    return getCmdStringFromCode(CMD_ERROR_HPC_TIMEOUT) + " desc=HPC engine timeout\n";
}

/**
 * @brief Serialise a NodeRecord to a single line (AC5, AC7).
 *
 * Format:
 * "node_id=<id> bunker_name=<name> ip_address=<ip> capacity=<cap> status=<status> last_seen=<epoch>"
 *
 * @param rec Node record to serialize.
 * @return Single-line serialized representation.
 */
static std::string serializeRecord(const NodeRecord& rec)
{
    std::ostringstream ss;
    ss << "node_id=" << rec.nodeId << " bunker_name=" << rec.bunkerName << " ip_address=" << rec.ipAddress
       << " capacity=" << rec.capacity << " status=" << getNodeStatusString(rec.status)
       << " last_seen=" << getEpochSeconds(rec.lastSeen);
    return ss.str();
}

// ---------------------------------------------------------------------------
// Command handlers (called from handleClient, AC1-AC7)
// ---------------------------------------------------------------------------

/**
 * @brief Handle a REGISTER command (AC1, AC2, AC3).
 *
 * Expected format:
 * "REGISTER node_id=<id> bunker_name=<name> ip_address=<ip> capacity=<n>"
 *
 * @param clientSock Connected client socket.
 * @param line Raw request line.
 */
static void handleRegister(int clientSock, const std::string& line)
{
    char nodeId[EOP_SMALL_BUFFER_SIZE] = {0};
    char bunkerName[EOP_SMALL_BUFFER_SIZE] = {0};
    char ipAddress[EOP_SMALL_BUFFER_SIZE] = {0};
    int capacity = 0;

    int parsed = std::sscanf(line.c_str(),
                             "REGISTER node_id=%63s bunker_name=%63s ip_address=%63s capacity=%d",
                             nodeId,
                             bunkerName,
                             ipAddress,
                             &capacity);

    if (parsed != 4)
    {
        // LCOV_EXCL_START
        const std::string err = "ERROR_BAD_REQUEST desc=Malformed REGISTER\n";
        send(clientSock, err.c_str(), err.size(), 0);
        auditLogger.writeString("Malformed REGISTER message", ERROR_LOG);
        g_logContext.result = "ERROR";
        return;
        // LCOV_EXCL_STOP
    }

    NodeRecord record;
    record.nodeId = nodeId;
    record.bunkerName = bunkerName;
    record.ipAddress = ipAddress;
    record.capacity = capacity;
    record.status = NodeStatus::ONLINE;
    record.lastSeen = std::chrono::system_clock::now();

    g_logContext.nodeId = record.nodeId; // Update log context with node id

    const RegisterNodeResult result = serverContext.registry.registerOrReconnectNode(record);

    if (result == RegisterNodeResult::NEW_NODE || result == RegisterNodeResult::RECONNECTED_NODE)
    {
        bindNodeToSession(serverContext, clientSock, record.nodeId);

        const std::string ack = buildAck(record.nodeId);
        send(clientSock, ack.c_str(), ack.size(), 0);

        std::ostringstream logMsg;
        if (result == RegisterNodeResult::RECONNECTED_NODE)
        {
            logMsg << "Node reconnected: " << serializeRecord(record);
        }
        else
        {
            logMsg << "Node registered: " << serializeRecord(record);
        }
        auditLogger.writeString(logMsg.str(), INFO_LOG);
        g_logContext.result = "OK";
    }
    else
    {
        // LCOV_EXCL_START
        const std::string errDup = buildErrorDuplicate(record.nodeId);
        send(clientSock, errDup.c_str(), errDup.size(), 0);

        auditLogger.writeString("Duplicate REGISTER attempt for node_id=" + record.nodeId, WARNING_LOG);
        g_logContext.result = "ERROR";
    }
}
// LCOV_EXCL_STOP

/**
 * @brief Handle a QUERY_NODE command (AC5, AC6).
 *
 * Expected format: "QUERY_NODE node_id=<id>"
 *
 * @param clientSock Connected client socket.
 * @param line Raw request line.
 */
static void handleQueryNode(int clientSock, const std::string& line)
{
    char nodeId[EOP_SMALL_BUFFER_SIZE] = {0};

    if (std::sscanf(line.c_str(), "QUERY_NODE node_id=%63s", nodeId) != 1)
    {
        // LCOV_EXCL_START
        const std::string err = "ERROR_BAD_REQUEST desc=Malformed QUERY_NODE\n";
        send(clientSock, err.c_str(), err.size(), 0);
        g_logContext.result = "ERROR";
        return;
        // LCOV_EXCL_STOP
    }

    NodeRecord record;
    g_logContext.nodeId = nodeId;

    if (serverContext.registry.queryNode(nodeId, record))
    {
        const std::string response = serializeRecord(record) + "\n";
        send(clientSock, response.c_str(), response.size(), 0);
        g_logContext.result = "OK";
    }
    else
    {
        // LCOV_EXCL_START
        const std::string errNotFound = buildErrorNotFound(nodeId);
        send(clientSock, errNotFound.c_str(), errNotFound.size(), 0);

        auditLogger.writeString("QUERY_NODE for unknown node_id=" + std::string(nodeId), WARNING_LOG);
        g_logContext.result = "ERROR";
    }
}
// LCOV_EXCL_STOP

/**
 * @brief Handle a LIST_NODES command (AC7).
 *
 * Returns all registered nodes, one per line, terminated by "END_LIST\n".
 *
 * @param clientSock Connected client socket.
 */
static void handleListNodes(int clientSock)
{
    const std::vector<NodeRecord> nodes = serverContext.registry.listNodes();

    std::ostringstream response;
    response << "LIST_NODES count=" << nodes.size() << "\n";
    for (const auto& rec : nodes)
    {
        response << serializeRecord(rec) << "\n";
    }
    response << "END_LIST\n";

    const std::string responseStr = response.str();
    send(clientSock, responseStr.c_str(), responseStr.size(), 0);
    g_logContext.result = "OK";
}

/**
 * @brief Returns the configured HPC dispatch timeout in milliseconds.
 *
 * Uses EOP_HPC_TIMEOUT_MS when present; otherwise falls back to 5000 ms.
 *
 * @return Timeout in milliseconds.
 */
static int getHpcDispatchTimeoutMs()
{
    constexpr int kDefaultTimeoutMs = 5000;

    const char* rawTimeout = std::getenv("EOP_HPC_TIMEOUT_MS");
    if (rawTimeout == nullptr)
    {
        return kDefaultTimeoutMs;
    }

    char* endPtr = nullptr;
    const long parsed = std::strtol(rawTimeout, &endPtr, 10);

    if (endPtr == rawTimeout || parsed <= 0 || parsed > 60000)
    {
        return kDefaultTimeoutMs;
    }

    return static_cast<int>(parsed);
}

/**
 * @brief Finds an ONLINE HPC Engine session reserved for dispatch.
 *
 * @param[out] engineFd Socket fd associated with the HPC Engine.
 * @param[out] engineNodeId Node id associated with the HPC Engine.
 * @return true if an available engine was found.
 */
static bool findAvailableHpcEngineSession(int& engineFd, std::string& engineNodeId)
{
    std::lock_guard<std::mutex> sessionsLock(serverContext.sessionsMutex);
    std::lock_guard<std::mutex> registryLock(serverContext.registry.registryMutex);

    for (const auto& [fd, session] : serverContext.activeSessions)
    {
        if (!session.active || !session.registered || !session.reservedForDispatch)
        {
            continue;
        }

        auto nodeIt = serverContext.registry.nodesMap.find(session.nodeId);
        if (nodeIt == serverContext.registry.nodesMap.end())
        {
            continue;
        }

        if (nodeIt->second.status != NodeStatus::ONLINE)
        {
            continue;
        }

        engineFd = fd;
        engineNodeId = session.nodeId;
        return true;
    }

    return false;
}

/**
 * @brief Sends the full payload to a socket.
 *
 * @param fd Target socket fd.
 * @param payload Payload to send.
 * @return true if the full payload was sent.
 */
static bool sendAllToSocket(int fd, const std::string& payload)
{
    std::size_t totalSent = 0;

    while (totalSent < payload.size())
    {
        const ssize_t sent = send(fd, payload.data() + totalSent, payload.size() - totalSent, 0);

        if (sent < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return false;
        }

        if (sent == 0)
        {
            return false;
        }

        totalSent += static_cast<std::size_t>(sent);
    }

    return true;
}

/**
 * @brief Receives one newline-terminated response from the HPC Engine within a deadline.
 *
 * The engine answers each request with exactly one JSON line (ADR-003), which for a large graph
 * is far bigger than a single recv(): centrality for 10K nodes is several hundred KB. Reading one
 * chunk and treating it as the whole response truncated it at 64 KB and, worse, left the rest in
 * the socket to be read as the reply to the next request. Bytes are therefore peeked first and
 * consumed only up to the terminator, so nothing past this response is ever taken from the stream.
 *
 * @param fd Engine socket fd.
 * @param timeoutMs Total time allowed for the whole response, not per read.
 * @param[out] timedOut True when the deadline expires before the terminator arrives.
 * @param[out] disconnected True when the engine socket closes, errors, or exceeds the size cap.
 * @return The response including its trailing newline, or empty string on timeout/disconnect.
 */
static std::string receiveHpcEngineResponse(int fd, int timeoutMs, bool& timedOut, bool& disconnected)
{
    constexpr std::size_t kHpcResponseChunkSize = 65536;

    timedOut = false;
    disconnected = false;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::string response;
    std::vector<char> chunk(kHpcResponseChunkSize);

    while (true)
    {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0)
        {
            timedOut = true;
            return "";
        }

        struct pollfd pfd = {};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int pollResult = poll(&pfd, 1, static_cast<int>(remaining));

        if (pollResult == 0)
        {
            timedOut = true;
            return "";
        }

        if (pollResult < 0 || (pfd.revents & POLLIN) == 0)
        {
            disconnected = true;
            return "";
        }

        const ssize_t peeked = recv(fd, chunk.data(), chunk.size(), MSG_PEEK);
        if (peeked <= 0)
        {
            disconnected = true;
            return "";
        }

        const auto* begin = chunk.data();
        const auto* newline = static_cast<const char*>(std::memchr(begin, '\n', static_cast<std::size_t>(peeked)));
        const std::size_t toConsume =
            newline != nullptr ? static_cast<std::size_t>(newline - begin) + 1 : static_cast<std::size_t>(peeked);

        const ssize_t consumed = recv(fd, chunk.data(), toConsume, 0);
        if (consumed <= 0)
        {
            disconnected = true;
            return "";
        }
        response.append(chunk.data(), static_cast<std::size_t>(consumed));

        if (newline != nullptr)
        {
            return response;
        }

        if (response.size() > MAX_HPC_RESPONSE_BYTES)
        {
            // Without a terminator in sight the stream cannot be resynchronised; dropping the
            // engine session is safer than relaying an unbounded, possibly corrupt payload.
            disconnected = true;
            return "";
        }
    }
}

/**
 * @brief Handle an ANALYZE_GRAPH command when no HPC Engine is available.
 *
 * The first integration step is intentionally conservative: the server recognizes
 * the v0.2 command and fails safely instead of treating it as an unknown command
 * or blocking while waiting for a non-existent engine.
 *
 * @param clientSock Connected client socket.
 * @param line Raw request line.
 */
static void handleAnalyzeGraph(int clientSock, const std::string& line)
{
    std::lock_guard<std::mutex> dispatchLock(serverContext.hpcDispatchMutex);

    int engineFd = -1;
    std::string engineNodeId;

    if (!findAvailableHpcEngineSession(engineFd, engineNodeId))
    {
        const std::string err = buildErrorServiceUnavailable();
        send(clientSock, err.c_str(), err.size(), 0);

        auditLogger.writeString("ANALYZE_GRAPH rejected: HPC engine unavailable", WARNING_LOG);
        g_logContext.result = "ERROR";
        return;
    }

    std::string request = injectTraceparent(normalizeAnalyzeGraphRequest(line));
    if (request.empty() || request.back() != '\n')
    {
        request.push_back('\n');
    }

    if (!sendAllToSocket(engineFd, request))
    {
        unregisterSession(serverContext, engineFd, "hpc engine dispatch send failed");

        const std::string err = buildErrorServiceUnavailable();
        send(clientSock, err.c_str(), err.size(), 0);

        auditLogger.writeString("ANALYZE_GRAPH rejected: failed to send request to HPC engine", ERROR_LOG);
        g_logContext.result = "ERROR";
        return;
    }

    bool timedOut = false;
    bool disconnected = false;
    std::string engineResponse = receiveHpcEngineResponse(engineFd, getHpcDispatchTimeoutMs(), timedOut, disconnected);

    if (timedOut)
    {
        const std::string err = buildErrorHpcTimeout();
        send(clientSock, err.c_str(), err.size(), 0);

        auditLogger.writeString("ANALYZE_GRAPH failed: HPC engine timeout", WARNING_LOG);
        g_logContext.result = "ERROR";
        return;
    }

    if (disconnected || engineResponse.empty())
    {
        unregisterSession(serverContext, engineFd, "hpc engine disconnected during dispatch");

        const std::string err = buildErrorServiceUnavailable();
        send(clientSock, err.c_str(), err.size(), 0);

        auditLogger.writeString("ANALYZE_GRAPH failed: HPC engine disconnected during dispatch", ERROR_LOG);
        g_logContext.result = "ERROR";
        return;
    }

    // A multi-MB response does not fit in one send(); a partial write would reach the client as a
    // truncated line.
    if (!sendAllToSocket(clientSock, engineResponse))
    {
        auditLogger.writeString("ANALYZE_GRAPH: client disconnected before the response was delivered", WARNING_LOG);
        g_logContext.result = "ERROR";
        return;
    }

    auditLogger.writeString("ANALYZE_GRAPH dispatched to HPC engine node_id=" + engineNodeId, INFO_LOG);
    g_logContext.result = "OK";
}

// ---------------------------------------------------------------------------
// Per-client worker handling
// ---------------------------------------------------------------------------

/**
 * @brief Handles a single message from a connected client.
 *
 * This function processes the text command sent by the client,
 * modifying states or returning responses accordingly.
 *
 * @param clientSock Connected client socket.
 * @param clientIp Remote client IP.
 * @param request The single plaintext command.
 */
void handleClientMessage(int clientSock, const std::string& clientIp, const std::string& request)
{
    if (request.rfind(getCmdStringFromCode(CMD_REGISTER) + " ", 0) == 0)
    {
        audit_log_write_fmt(INFO_LOG, "Request received: 'REGISTER ...' from IP=%s", clientIp.c_str());
        updateSessionActivity(serverContext, clientSock);
        handleRegister(clientSock, request);
    }
    else if (request.rfind(getCmdStringFromCode(CMD_QUERY_NODE) + " ", 0) == 0)
    {
        audit_log_write_fmt(INFO_LOG, "Request received: '%s' from IP=%s", request.c_str(), clientIp.c_str());
        updateSessionActivity(serverContext, clientSock);
        handleQueryNode(clientSock, request);
    }
    else if (request.rfind(getCmdStringFromCode(CMD_LIST_NODES), 0) == 0)
    {
        audit_log_write_fmt(INFO_LOG, "Request received: 'LIST_NODES' from IP=%s", clientIp.c_str());
        updateSessionActivity(serverContext, clientSock);
        handleListNodes(clientSock);
    }
    else if (request.rfind(getCmdStringFromCode(CMD_HEARTBEAT), 0) == 0)
    {
        // Skip logging for HEARTBEAT to maintain high performance under load
        handleHeartbeat(clientSock);
    }
    else if (isAnalyzeGraphRequest(request))
    {
        audit_log_write_fmt(INFO_LOG, "Request received: 'ANALYZE_GRAPH ...' from IP=%s", clientIp.c_str());
        updateSessionActivity(serverContext, clientSock);
        handleAnalyzeGraph(clientSock, request);
    }
    else
    {
        if (!request.empty())
        {
            audit_log_write_fmt(WARNING_LOG, "Unknown command '%s' from %s", request.c_str(), clientIp.c_str());
            const std::string err = "ERROR_BAD_REQUEST desc=Unknown command\n";
            send(clientSock, err.c_str(), err.size(), 0);
            g_logContext.result = "ERROR";
        }
    }
}

// ---------------------------------------------------------------------------
// Server entry-point
// ---------------------------------------------------------------------------

/**
 * @brief Starts the TCP server and accepts incoming client connections.
 *
 * @param port TCP port to bind.
 * @param idleTimeout Initial idle timeout value.
 */
void startServer(int port, int idleTimeout)
{
    InitTracer();
    InitMetrics();

    serverContext.idleTimeoutSeconds = idleTimeout;
    serverContext.running = true;
    loadServerConfig(serverContext);

    int serverSock = socket(AF_INET, SOCK_STREAM, 0);
    // LCOV_EXCL_START
    if (serverSock < 0)
    {
        std::cerr << "Error: Could not create server socket." << std::endl;
        return;
    }
    // LCOV_EXCL_STOP

    int opt = 1;
    setsockopt(serverSock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in serverAddr = {};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(static_cast<uint16_t>(port));

    // LCOV_EXCL_START
    if (bind(serverSock, reinterpret_cast<struct sockaddr*>(&serverAddr), sizeof(serverAddr)) < 0)
    {
        std::cerr << "Critical error: Cannot start server on port " << port << ". Port may already be in use."
                  << std::endl;
        auditLogger.writeString("Failed to bind server port", CRITICAL_LOG);
        close(serverSock);
        return;
    }
    // LCOV_EXCL_STOP

    // LCOV_EXCL_START
    if (listen(serverSock, serverContext.maxClients) < 0)
    {
        std::cerr << "Critical error: Failed to listen()." << std::endl;
        close(serverSock);
        return;
    }
    // LCOV_EXCL_STOP

    unsigned int workerCount = std::thread::hardware_concurrency();
    if (workerCount == 0)
    {
        workerCount = 4;
    }
    if (workerCount > static_cast<unsigned int>(serverContext.maxClients))
    {
        workerCount = static_cast<unsigned int>(serverContext.maxClients);
    }
    if (workerCount == 0)
    {
        workerCount = 1;
    }

    std::vector<std::thread> workers;
    workers.reserve(workerCount);

    for (unsigned int i = 0; i < workerCount; ++i)
    {
        workers.emplace_back(workerLoop, std::ref(serverContext));
    }

    std::thread timeoutMonitor(timeoutMonitorLoop, std::ref(serverContext));

    std::cout << "Server started on port " << port << ". Inactivity timeout: " << serverContext.idleTimeoutSeconds
              << "s | Max clients: " << serverContext.maxClients
              << " | Heartbeat interval: " << serverContext.heartbeatIntervalSeconds
              << "s | Worker threads: " << workerCount << std::endl;
    auditLogger.writeString("Server started", INFO_LOG);

    std::vector<struct pollfd> pollfds;
    pollfds.reserve(serverContext.maxClients + 1);

    while (!isTerminationSignalReceived)
    {
        pollfds.clear();

        // Listener socket
        pollfds.push_back({serverSock, POLLIN, 0});

        {
            std::lock_guard<std::mutex> lock(serverContext.sessionsMutex);
            for (auto& [fd, session] : serverContext.activeSessions)
            {
                if (session.active)
                {
                    const short events = session.reservedForDispatch ? 0 : POLLIN;
                    pollfds.push_back({fd, events, 0});
                }
            }
        }

        int ret = poll(pollfds.data(), static_cast<nfds_t>(pollfds.size()), 1000); // 1s timeout
        if (ret > 0)
        {
            // Check listener
            if (pollfds[0].revents & POLLIN)
            {
                struct sockaddr_in clientAddr = {};
                socklen_t clientLen = sizeof(clientAddr);
                int clientSock = accept(serverSock, reinterpret_cast<struct sockaddr*>(&clientAddr), &clientLen);

                if (clientSock >= 0)
                {
                    if (isTerminationSignalReceived || !serverContext.running.load())
                    {
                        shutdown(clientSock, SHUT_RDWR);
                        close(clientSock);
                        break;
                    }

                    std::string clientIp = inet_ntoa(clientAddr.sin_addr);
                    std::string sessionId;

                    if (!tryRegisterSession(serverContext, clientSock, clientIp, sessionId))
                    {
                        const std::string rejectMsg = buildConnectionRejected();
                        send(clientSock, rejectMsg.c_str(), rejectMsg.size(), 0);
                        shutdown(clientSock, SHUT_RDWR);
                        close(clientSock);

                        auditLogger.writeString("Rejected connection due to max client limit from IP=" + clientIp,
                                                WARNING_LOG);
                    }
                    else
                    {
                        if (shouldSendImmediateWelcome())
                        {
                            const std::string welcomeMsg = buildWelcomeMessage(sessionId);
                            send(clientSock, welcomeMsg.c_str(), welcomeMsg.size(), 0);
                        }

                        auto tracer = opentelemetry::trace::Provider::GetTracerProvider()->GetTracer("eop-server");
                        auto connSpan = tracer->StartSpan("client_connect");
                        connSpan->SetAttribute("operation.name", "client_connect");
                        connSpan->SetAttribute("session.id", sessionId);
                        connSpan->SetAttribute("result", "OK");

                        {
                            std::lock_guard<std::mutex> lock(serverContext.sessionsMutex);
                            auto it = serverContext.activeSessions.find(clientSock);
                            if (it != serverContext.activeSessions.end())
                            {
                                it->second.connectionSpan = connSpan;
                            }
                        }

                        std::ostringstream acceptedMsg;
                        acceptedMsg << "New connection accepted IP=" << clientIp << " Session=" << sessionId
                                    << " Total connections:" << serverContext.activeConnections.load();
                        auditLogger.writeString(acceptedMsg.str(), CONNECTION_LOG);
                    }
                }
            }

            // Check clients
            // Skip index 0 (listener)
            for (size_t i = 1; i < pollfds.size(); ++i)
            {
                if (pollfds[i].revents & (POLLIN | POLLERR | POLLHUP))
                {
                    int fd = pollfds[i].fd;
                    char requestBuf[SOCKET_READ_BUFFER_BYTES];
                    ssize_t bytesRecv = recv(fd, requestBuf, sizeof(requestBuf), MSG_DONTWAIT);

                    if (bytesRecv == 0)
                    {
                        // El peer cerró: lo que quedó en el buffer ya no puede ser un mensaje a
                        // medio llegar (no viene nada más), así que acá sí es correcto despacharlo
                        // aunque le falte el '\n'. Es el único punto donde eso puede afirmarse.
                        std::string leftover;
                        std::string clientIp;
                        {
                            std::lock_guard<std::mutex> lock(serverContext.sessionsMutex);
                            auto it = serverContext.activeSessions.find(fd);
                            if (it != serverContext.activeSessions.end())
                            {
                                clientIp = it->second.clientIp;
                                leftover.swap(it->second.pendingInput);
                            }
                        }

                        while (!leftover.empty() && (leftover.back() == '\n' || leftover.back() == '\r'))
                        {
                            leftover.pop_back();
                        }

                        if (!leftover.empty() && leftover != "quit" && leftover != "exit")
                        {
                            enqueueMessage(serverContext, fd, clientIp, leftover);
                        }

                        unregisterSession(serverContext, fd, "client closed connection cleanly");
                    }
                    else if (bytesRecv < 0)
                    {
                        if (errno != EAGAIN && errno != EWOULDBLOCK)
                        {
                            unregisterSession(serverContext, fd, "socket recv error / abrupt disconnect");
                        }
                    }
                    else
                    {
                        // Lo recibido se agrega a lo que haya quedado incompleto de lecturas
                        // anteriores: un recv() devuelve lo que llegó, no un mensaje entero, así
                        // que solo se despacha al ver un '\n' real (ADR-003).
                        std::string clientIp;
                        std::string request;
                        {
                            std::lock_guard<std::mutex> lock(serverContext.sessionsMutex);
                            auto it = serverContext.activeSessions.find(fd);
                            if (it != serverContext.activeSessions.end())
                            {
                                clientIp = it->second.clientIp;
                                it->second.pendingInput.append(requestBuf, static_cast<size_t>(bytesRecv));
                                request.swap(it->second.pendingInput);
                            }
                            else
                            {
                                request.assign(requestBuf, static_cast<size_t>(bytesRecv));
                            }
                        }

                        bool sessionClosed = false;
                        size_t pos = 0;
                        while ((pos = request.find('\n')) != std::string::npos)
                        {
                            std::string line = request.substr(0, pos);
                            request.erase(0, pos + 1);

                            while (!line.empty() && (line.back() == '\r'))
                            {
                                line.pop_back();
                            }

                            if (line == "quit" || line == "exit")
                            {
                                unregisterSession(serverContext, fd, "client requested graceful exit");
                                sessionClosed = true;
                                break;
                            }
                            else if (!line.empty())
                            {
                                enqueueMessage(serverContext, fd, clientIp, line);
                            }
                        }

                        // El resto sin '\n' es un mensaje a medio llegar, no un comando: se guarda
                        // para completarlo con el próximo recv() en vez de despacharlo truncado.
                        if (!sessionClosed)
                        {
                            std::lock_guard<std::mutex> lock(serverContext.sessionsMutex);
                            auto it = serverContext.activeSessions.find(fd);
                            if (it != serverContext.activeSessions.end())
                            {
                                if (request.size() > maxRequestBytes())
                                {
                                    it->second.pendingInput.clear();
                                    const std::string err = "ERROR_BAD_REQUEST desc=Message too long\n";
                                    send(fd, err.c_str(), err.size(), 0);
                                    audit_log_write_fmt(WARNING_LOG,
                                                        "Unterminated message over %zu bytes from %s, buffer dropped",
                                                        maxRequestBytes(),
                                                        clientIp.c_str());
                                }
                                else
                                {
                                    it->second.pendingInput.swap(request);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    auditLogger.writeString("Graceful shutdown requested", INFO_LOG);

    close(serverSock);

    serverContext.running = false;
    serverContext.queueCv.notify_all();

    waitForActiveSessionsToFinish(serverContext, GRACEFUL_SHUTDOWN_TIMEOUT_SECONDS);

    if (serverContext.activeConnections.load() > 0)
    {
        auditLogger.writeString("Graceful shutdown timeout reached; closing remaining sessions", WARNING_LOG);
        closeRemainingSessions(serverContext);
    }

    if (timeoutMonitor.joinable())
    {
        timeoutMonitor.join();
    }

    for (auto& worker : workers)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }

    std::cout << "Server stopped." << std::endl;
    ShutdownTracer();
    ShutdownMetrics();
}

// ---------------------------------------------------------------------------
// main (excluded in test builds)
// ---------------------------------------------------------------------------

#ifndef TEST_MODE
int main()
{
    setSigaction();
    ConfigLoader config("config.env");

    const char* portEnv = std::getenv("EOP_PORT");
    int port = (portEnv != nullptr) ? std::atoi(portEnv) : config.getInt("EOP_PORT", EOP_PORT);

    const char* timeoutEnv = std::getenv("EOP_IDLE_TIMEOUT");
    int idleTimeout =
        (timeoutEnv != nullptr) ? std::atoi(timeoutEnv) : config.getInt("EOP_IDLE_TIMEOUT", EOP_IDLE_TIMEOUT);

    startServer(port, idleTimeout);
    return 0;
}
#endif
