package telemetry

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"net/http/httptest"
	"strings"
	"testing"

	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/propagation"
	sdktrace "go.opentelemetry.io/otel/sdk/trace"
)

func TestLoggerWritesADR019Fields(t *testing.T) {
	var buf bytes.Buffer
	logger := NewLogger(&buf, "api-gateway")

	provider := sdktrace.NewTracerProvider()
	defer func() { _ = provider.Shutdown(context.Background()) }()
	ctx, span := provider.Tracer("test").Start(context.Background(), "op")
	logger.With("node_id", "alpha").InfoContext(ctx, "node queried")
	span.End()

	var line map[string]any
	if err := json.Unmarshal(buf.Bytes(), &line); err != nil {
		t.Fatalf("not JSON: %v\n%s", err, buf.String())
	}
	sc := span.SpanContext()
	want := map[string]any{
		"level": "INFO", "service": "api-gateway", "message": "node queried", "node_id": "alpha",
		"trace_id": sc.TraceID().String(), "span_id": sc.SpanID().String(),
	}
	for key, value := range want {
		if line[key] != value {
			t.Errorf("%s = %v, want %v", key, line[key], value)
		}
	}
	if _, ok := line["timestamp"].(string); !ok {
		t.Error("timestamp missing")
	}
	if _, ok := line["time"]; ok {
		t.Error("slog's default time key must be renamed to timestamp")
	}
}

func TestLoggerGroupsNestLaterAttributes(t *testing.T) {
	var buf bytes.Buffer
	NewLogger(&buf, "api-gateway").WithGroup("extra").Info("grouped", "k", "v")
	if !strings.Contains(buf.String(), `"extra":{"k":"v"}`) {
		t.Fatalf("group not applied: %s", buf.String())
	}
}

func TestLoggerWithoutSpanOmitsTraceFields(t *testing.T) {
	var buf bytes.Buffer
	NewLogger(&buf, "api-gateway").Info("started")
	if strings.Contains(buf.String(), "trace_id") {
		t.Fatalf("trace_id written without a span: %s", buf.String())
	}
}

func TestMetricsExposition(t *testing.T) {
	m := NewMetrics("1.0.0", "canary")
	m.Observe("GET", "/v1/nodes", 200, 0.12)
	m.Observe("GET", "/v1/nodes", 503, 0.01)

	rec := httptest.NewRecorder()
	m.Handler().ServeHTTP(rec, httptest.NewRequest("GET", "/metrics", nil))
	body, _ := io.ReadAll(rec.Body)
	text := string(body)
	for _, want := range []string{
		`eop_gateway_http_requests_total{method="GET",route="/v1/nodes",status_class="2xx",track="canary",version="1.0.0"} 1`,
		`eop_gateway_http_requests_total{method="GET",route="/v1/nodes",status_class="5xx",track="canary",version="1.0.0"} 1`,
		`eop_gateway_http_request_duration_seconds_bucket{method="GET",route="/v1/nodes",track="canary",version="1.0.0",le="0.3"} 2`,
		"go_goroutines",
	} {
		if !strings.Contains(text, want) {
			t.Errorf("exposition missing %q", want)
		}
	}
}

func TestCircuitMetrics(t *testing.T) {
	m := NewMetrics("1.0.0", "stable")
	m.SetCircuitState("ai", 2)
	m.CircuitTransition("ai", "closed", "open")
	m.Fallback("ai", "stale")
	rec := httptest.NewRecorder()
	m.Handler().ServeHTTP(rec, httptest.NewRequest("GET", "/metrics", nil))
	text := rec.Body.String()
	for _, want := range []string{
		`eop_gateway_circuit_state{service="ai",track="stable",version="1.0.0"} 2`,
		`eop_circuit_breaker_state_transitions_total{from_state="closed",service="ai",to_state="open",track="stable",version="1.0.0"} 1`,
		`eop_gateway_fallback_responses_total{kind="stale",service="ai",track="stable",version="1.0.0"} 1`,
	} {
		if !strings.Contains(text, want) {
			t.Errorf("exposition missing %q", want)
		}
	}
}

func TestStatusClass(t *testing.T) {
	for status, want := range map[int]string{200: "2xx", 404: "4xx", 503: "5xx"} {
		if got := statusClass(status); got != want {
			t.Errorf("statusClass(%d) = %s, want %s", status, got, want)
		}
	}
}

func TestSetupTracing(t *testing.T) {
	previous := otel.GetTracerProvider()
	t.Cleanup(func() { otel.SetTracerProvider(previous) })

	shutdown, err := SetupTracing(context.Background(), TracingConfig{Enabled: false})
	if err != nil || shutdown(context.Background()) != nil {
		t.Fatalf("disabled tracing: %v", err)
	}
	if _, ok := otel.GetTextMapPropagator().(propagation.TraceContext); !ok {
		t.Fatal("the W3C propagator must be installed even with tracing disabled")
	}

	t.Setenv("OTEL_EXPORTER_OTLP_ENDPOINT", "http://127.0.0.1:1")
	shutdown, err = SetupTracing(context.Background(), TracingConfig{Enabled: true, ServiceName: "api-gateway", Version: "1.0.0", Track: "stable"})
	if err != nil {
		t.Fatalf("enabled tracing: %v", err)
	}
	if _, ok := otel.GetTracerProvider().(*sdktrace.TracerProvider); !ok {
		t.Fatal("enabled tracing must install an SDK provider")
	}
	if err := shutdown(context.Background()); err != nil {
		t.Fatalf("shutdown with no spans: %v", err)
	}
}
