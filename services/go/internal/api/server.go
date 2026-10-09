package api

import (
	"context"
	"log/slog"
	"net/http"
	"time"

	"github.com/go-chi/chi/v5"
	"go.opentelemetry.io/contrib/instrumentation/net/http/otelhttp"
	"go.opentelemetry.io/otel/trace"

	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/eopclient"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/telemetry"
)

// NodeStore is what the API needs from eop-server. *eopclient.Client implements it.
type NodeStore interface {
	ListNodes(ctx context.Context) ([]eopclient.Node, error)
	QueryNode(ctx context.Context, nodeID string) (eopclient.Node, error)
	Heartbeat(ctx context.Context) error
}

// Options are the Server's dependencies.
type Options struct {
	Store         NodeStore
	Logger        *slog.Logger
	Metrics       *telemetry.Metrics
	OpenAPIJSON   []byte
	HealthTimeout time.Duration
}

// Server serves the public REST API.
type Server struct {
	store         NodeStore
	logger        *slog.Logger
	metrics       *telemetry.Metrics
	openAPI       []byte
	healthTimeout time.Duration
	now           func() time.Time
}

// NewServer builds the API server.
func NewServer(opts Options) *Server {
	return &Server{
		store:         opts.Store,
		logger:        opts.Logger,
		metrics:       opts.Metrics,
		openAPI:       opts.OpenAPIJSON,
		healthTimeout: opts.HealthTimeout,
		now:           time.Now,
	}
}

// Routes returns the HTTP handler with every /v1 route, traced with OpenTelemetry.
func (s *Server) Routes() http.Handler {
	// The span is opened before routing; observe renames it to the route pattern once known.
	return otelhttp.NewHandler(s.router(), "http.request")
}

func (s *Server) router() *chi.Mux {
	r := chi.NewRouter()
	// observe wraps recoverPanics so a recovered panic is still counted and logged as a 500.
	r.Use(s.observe, s.recoverPanics)
	r.NotFound(func(w http.ResponseWriter, r *http.Request) {
		s.writeError(w, r, http.StatusNotFound, errorBody{Code: CodeRouteNotFound, Message: "no such route"})
	})
	r.MethodNotAllowed(func(w http.ResponseWriter, r *http.Request) {
		s.writeError(w, r, http.StatusMethodNotAllowed, errorBody{Code: CodeMethodNotAllowed, Message: "method not allowed on this route"})
	})
	r.Route("/v1", func(r chi.Router) {
		r.Get("/nodes", s.listNodes)
		r.Get("/nodes/{node_id}", s.getNode)
		r.Get("/health", s.health)
		r.Get("/openapi.json", s.openAPISpec)
	})
	return r
}

// observe records metrics and one access log line per request, keyed by route pattern.
func (s *Server) observe(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := s.now()
		recorder := &statusRecorder{ResponseWriter: w, status: http.StatusOK}
		next.ServeHTTP(recorder, r)

		route := chi.RouteContext(r.Context()).RoutePattern()
		if route == "" {
			route = "unmatched"
		}
		elapsed := s.now().Sub(start)
		trace.SpanFromContext(r.Context()).SetName(r.Method + " " + route)
		if s.metrics != nil {
			s.metrics.Observe(r.Method, route, recorder.status, elapsed.Seconds())
		}
		level := slog.LevelInfo
		if recorder.status >= http.StatusInternalServerError {
			level = slog.LevelError
		}
		s.logger.Log(r.Context(), level, "request served",
			"method", r.Method, "route", route, "status", recorder.status,
			"duration_ms", float64(elapsed.Microseconds())/1000)
	})
}

// recoverPanics turns a handler panic into a 500 with the standard error body.
func (s *Server) recoverPanics(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		defer func() {
			if v := recover(); v != nil {
				if v == http.ErrAbortHandler {
					panic(v)
				}
				s.logger.ErrorContext(r.Context(), "handler panic", "panic", v)
				s.writeError(w, r, http.StatusInternalServerError, errorBody{Code: CodeInternal, Message: "internal error"})
			}
		}()
		next.ServeHTTP(w, r)
	})
}

type statusRecorder struct {
	http.ResponseWriter
	status int
}

func (rec *statusRecorder) WriteHeader(status int) {
	rec.status = status
	rec.ResponseWriter.WriteHeader(status)
}
