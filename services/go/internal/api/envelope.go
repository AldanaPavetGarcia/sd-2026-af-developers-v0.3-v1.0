// Package api is the gateway's HTTP layer: routes, the response envelope and the mapping from
// eop-server answers to HTTP status codes (ADR-010 §3).
package api

import (
	"encoding/json"
	"net/http"
	"time"

	"go.opentelemetry.io/otel/trace"
)

const contentTypeJSON = "application/json"

// Error codes are part of the public contract: clients may branch on them.
const (
	CodeInvalidNodeID       = "INVALID_NODE_ID"
	CodeNodeNotFound        = "NODE_NOT_FOUND"
	CodeUpstreamUnavailable = "UPSTREAM_UNAVAILABLE"
	CodeUpstreamError       = "UPSTREAM_ERROR"
	CodeRouteNotFound       = "ROUTE_NOT_FOUND"
	CodeMethodNotAllowed    = "METHOD_NOT_ALLOWED"
	CodeInternal            = "INTERNAL_ERROR"
)

type meta struct {
	Timestamp string `json:"timestamp"`
	TraceID   string `json:"trace_id,omitempty"`
}

type dataEnvelope struct {
	Data any  `json:"data"`
	Meta meta `json:"meta"`
}

type errorBody struct {
	Code    string `json:"code"`
	Message string `json:"message"`
	Field   string `json:"field,omitempty"`
}

type errorEnvelope struct {
	Error errorBody `json:"error"`
	Meta  meta      `json:"meta"`
}

func (s *Server) meta(r *http.Request) meta {
	m := meta{Timestamp: s.now().UTC().Format(time.RFC3339Nano)}
	if sc := trace.SpanContextFromContext(r.Context()); sc.IsValid() {
		m.TraceID = sc.TraceID().String()
	}
	return m
}

func (s *Server) writeData(w http.ResponseWriter, r *http.Request, status int, data any) {
	s.writeJSON(w, r, status, dataEnvelope{Data: data, Meta: s.meta(r)})
}

func (s *Server) writeError(w http.ResponseWriter, r *http.Request, status int, body errorBody) {
	s.writeJSON(w, r, status, errorEnvelope{Error: body, Meta: s.meta(r)})
}

func (s *Server) writeJSON(w http.ResponseWriter, r *http.Request, status int, v any) {
	payload, err := json.Marshal(v)
	if err != nil {
		s.logger.ErrorContext(r.Context(), "encoding response", "error", err)
		status = http.StatusInternalServerError
		payload = []byte(`{"error":{"code":"` + CodeInternal + `","message":"internal error"}}`)
	}
	w.Header().Set("Content-Type", contentTypeJSON)
	w.WriteHeader(status)
	if _, err := w.Write(payload); err != nil {
		s.logger.DebugContext(r.Context(), "writing response", "error", err)
	}
}
