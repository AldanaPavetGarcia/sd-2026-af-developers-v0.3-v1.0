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
	registry *prometheus.Registry
	requests *prometheus.CounterVec
	duration *prometheus.HistogramVec
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
	registry.MustRegister(m.requests, m.duration,
		collectors.NewGoCollector(), collectors.NewProcessCollector(collectors.ProcessCollectorOpts{}))
	return m
}

// Observe records one served request. route is the route pattern, never the raw path, so
// node ids do not create one series each.
func (m *Metrics) Observe(method, route string, status int, seconds float64) {
	m.requests.WithLabelValues(method, route, statusClass(status)).Inc()
	m.duration.WithLabelValues(method, route).Observe(seconds)
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
