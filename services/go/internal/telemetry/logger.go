// Package telemetry wires logs, metrics and traces for the gateway (ADR-005, ADR-019, ADR-010).
package telemetry

import (
	"context"
	"io"
	"log/slog"
	"time"

	"go.opentelemetry.io/otel/trace"
)

// NewLogger returns a JSON logger whose lines carry the ADR-019 fields: timestamp, level,
// service, trace_id, span_id and message.
func NewLogger(w io.Writer, service string) *slog.Logger {
	handler := slog.NewJSONHandler(w, &slog.HandlerOptions{ReplaceAttr: renameStandardKeys})
	return slog.New(traceHandler{Handler: handler}).With("service", service)
}

func renameStandardKeys(groups []string, a slog.Attr) slog.Attr {
	if len(groups) > 0 {
		return a
	}
	switch a.Key {
	case slog.TimeKey:
		return slog.String("timestamp", a.Value.Time().UTC().Format(time.RFC3339Nano))
	case slog.MessageKey:
		a.Key = "message"
	}
	return a
}

// traceHandler adds trace_id and span_id from the record's context, so a log line written while
// serving a request can be joined to its trace (US-306).
type traceHandler struct{ slog.Handler }

func (h traceHandler) Handle(ctx context.Context, record slog.Record) error {
	if sc := trace.SpanContextFromContext(ctx); sc.IsValid() {
		record.AddAttrs(slog.String("trace_id", sc.TraceID().String()), slog.String("span_id", sc.SpanID().String()))
	}
	return h.Handler.Handle(ctx, record)
}

func (h traceHandler) WithAttrs(attrs []slog.Attr) slog.Handler {
	return traceHandler{Handler: h.Handler.WithAttrs(attrs)}
}

// WithGroup nests every attribute added afterwards, trace_id and span_id included, so Loki
// would not find them at the top level. The gateway never groups attributes.
func (h traceHandler) WithGroup(name string) slog.Handler {
	return traceHandler{Handler: h.Handler.WithGroup(name)}
}
