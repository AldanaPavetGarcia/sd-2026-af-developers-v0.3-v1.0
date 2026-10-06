package telemetry

import (
	"context"

	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/attribute"
	"go.opentelemetry.io/otel/exporters/otlp/otlptrace/otlptracehttp"
	"go.opentelemetry.io/otel/propagation"
	"go.opentelemetry.io/otel/sdk/resource"
	sdktrace "go.opentelemetry.io/otel/sdk/trace"
	semconv "go.opentelemetry.io/otel/semconv/v1.26.0"
)

// TracingConfig describes the gateway's identity in traces.
type TracingConfig struct {
	Enabled     bool
	ServiceName string
	Version     string
	Track       string
}

// SetupTracing installs the W3C trace-context propagator and, when enabled, an OTLP/HTTP
// exporter. The endpoint comes from the standard OTEL_EXPORTER_OTLP_* variables, the same ones
// eop-server uses. The returned function flushes pending spans.
func SetupTracing(ctx context.Context, cfg TracingConfig) (func(context.Context) error, error) {
	otel.SetTextMapPropagator(propagation.TraceContext{})
	if !cfg.Enabled {
		return func(context.Context) error { return nil }, nil
	}
	exporter, err := otlptracehttp.New(ctx)
	if err != nil {
		return nil, err
	}
	res := resource.NewWithAttributes(semconv.SchemaURL,
		semconv.ServiceName(cfg.ServiceName),
		semconv.ServiceVersion(cfg.Version),
		attribute.String("deployment.track", cfg.Track),
	)
	provider := sdktrace.NewTracerProvider(sdktrace.WithBatcher(exporter), sdktrace.WithResource(res))
	otel.SetTracerProvider(provider)
	return provider.Shutdown, nil
}
