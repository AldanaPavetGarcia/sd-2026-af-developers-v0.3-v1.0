package traffic

import "github.com/prometheus/client_golang/prometheus"

// Results of one request.
const (
	resultOK    = "ok"
	resultError = "error"
)

// Targets and operations, as metric label values.
const (
	targetGateway = "gateway"
	targetServer  = "eop-server"
	opRegister    = "register"
	opHeartbeat   = "heartbeat"
	opAnalyze     = "graph_analyze"
)

// Metrics: what the run did, seen from the client side, next to the services' own metrics.
type Metrics struct {
	requests *prometheus.CounterVec
	duration *prometheus.HistogramVec
	online   prometheus.Gauge
}

// NewMetrics registers the generator's metrics in reg.
func NewMetrics(reg prometheus.Registerer) *Metrics {
	m := &Metrics{
		requests: prometheus.NewCounterVec(prometheus.CounterOpts{
			Name: "eop_traffic_requests_total",
			Help: "Requests of the SLO traffic generator, by target, operation and result (ok or error).",
		}, []string{"target", "op", "result"}),
		duration: prometheus.NewHistogramVec(prometheus.HistogramOpts{
			Name:    "eop_traffic_request_duration_seconds",
			Help:    "Client-side duration of the SLO traffic generator's requests.",
			Buckets: []float64{0.005, 0.01, 0.025, 0.05, 0.1, 0.2, 0.3, 0.5, 1, 2, 5},
		}, []string{"target", "op"}),
		online: prometheus.NewGauge(prometheus.GaugeOpts{
			Name: "eop_traffic_nodes_registered",
			Help: "Synthetic bunker nodes currently registered on eop-server.",
		}),
	}
	reg.MustRegister(m.requests, m.duration, m.online)
	return m
}

func (m *Metrics) observe(target, op string, seconds float64, err error) {
	result := resultOK
	if err != nil {
		result = resultError
	}
	m.requests.WithLabelValues(target, op, result).Inc()
	m.duration.WithLabelValues(target, op).Observe(seconds)
}
