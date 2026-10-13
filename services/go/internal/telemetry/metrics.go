package telemetry

import (
	"net/http"
	"strconv"

	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/collectors"
	"github.com/prometheus/client_golang/prometheus/promhttp"
)

// latencyBuckets put boundaries around the 300 ms latency objective (ADR-010 §6) so the P99
// estimate is precise where the SLO is decided.
var latencyBuckets = []float64{0.005, 0.01, 0.025, 0.05, 0.1, 0.15, 0.2, 0.25, 0.3, 0.4, 0.5, 1, 2.5, 5, 10}

// Metrics holds the gateway's Prometheus collectors on a private registry.
type Metrics struct {
	registry     *prometheus.Registry
	requests     *prometheus.CounterVec
	duration     *prometheus.HistogramVec
	circuitState *prometheus.GaugeVec
	transitions  *prometheus.CounterVec
	fallbacks    *prometheus.CounterVec
}

// NewMetrics registers the HTTP metrics. version and track are constant labels so canary and
// stable pods are told apart (ADR-011, US-304 AC3).
func NewMetrics(version, track string) *Metrics {
	registry := prometheus.NewRegistry()
	constLabels := prometheus.Labels{"version": version, "track": track}
	m := &Metrics{
		registry: registry,
		requests: prometheus.NewCounterVec(prometheus.CounterOpts{
			Name:        "eop_gateway_http_requests_total",
			Help:        "HTTP requests served by the API gateway.",
			ConstLabels: constLabels,
		}, []string{"method", "route", "status_class"}),
		duration: prometheus.NewHistogramVec(prometheus.HistogramOpts{
			Name:        "eop_gateway_http_request_duration_seconds",
			Help:        "Time to serve an HTTP request, measured in the gateway.",
			ConstLabels: constLabels,
			Buckets:     latencyBuckets,
		}, []string{"method", "route"}),
	}
	// Circuit breaker metrics (US-303 AC5, ADR-012 §5). The transition counter keeps the name the
	// PRD reference ADR-012 gives it.
	m.circuitState = prometheus.NewGaugeVec(prometheus.GaugeOpts{
		Name:        "eop_gateway_circuit_state",
		Help:        "Circuit breaker state per external service: 0 closed, 1 half-open, 2 open.",
		ConstLabels: constLabels,
	}, []string{"service"})
	m.transitions = prometheus.NewCounterVec(prometheus.CounterOpts{
		Name:        "eop_circuit_breaker_state_transitions_total",
		Help:        "Circuit breaker state changes per external service.",
		ConstLabels: constLabels,
	}, []string{"service", "from_state", "to_state"})
	m.fallbacks = prometheus.NewCounterVec(prometheus.CounterOpts{
		Name:        "eop_gateway_fallback_responses_total",
		Help:        "Responses served without the external service: stale cached data or a degraded answer.",
		ConstLabels: constLabels,
	}, []string{"service", "kind"})
	registry.MustRegister(m.requests, m.duration, m.circuitState, m.transitions, m.fallbacks,
		collectors.NewGoCollector(), collectors.NewProcessCollector(collectors.ProcessCollectorOpts{}))
	return m
}

// Observe records one served request. route is the route pattern, never the raw path, so
// node ids do not create one series each.
func (m *Metrics) Observe(method, route string, status int, seconds float64) {
	m.requests.WithLabelValues(method, route, statusClass(status)).Inc()
	m.duration.WithLabelValues(method, route).Observe(seconds)
}

// SetCircuitState publishes the current state of a circuit (0 closed, 1 half-open, 2 open).
func (m *Metrics) SetCircuitState(service string, state float64) {
	m.circuitState.WithLabelValues(service).Set(state)
}

// CircuitTransition counts one state change of a circuit.
func (m *Metrics) CircuitTransition(service, from, to string) {
	m.transitions.WithLabelValues(service, from, to).Inc()
}

// Fallback counts one response served without the external service; kind is "stale" or
// "degraded".
func (m *Metrics) Fallback(service, kind string) {
	m.fallbacks.WithLabelValues(service, kind).Inc()
}

// RequestCounter returns the request counter for one label set, for tests and checks.
func (m *Metrics) RequestCounter(method, route, statusClass string) prometheus.Counter {
	return m.requests.WithLabelValues(method, route, statusClass)
}

// Handler serves the registry in the Prometheus exposition format.
func (m *Metrics) Handler() http.Handler {
	return promhttp.HandlerFor(m.registry, promhttp.HandlerOpts{Registry: m.registry})
}

func statusClass(status int) string {
	return strconv.Itoa(status/100) + "xx"
}
