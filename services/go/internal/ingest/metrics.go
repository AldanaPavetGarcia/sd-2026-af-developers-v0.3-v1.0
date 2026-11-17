package ingest

import (
	"time"

	"github.com/prometheus/client_golang/prometheus"

	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/edge"
)

// Buckets of the end-to-end latency: the NFR is P99 < 500 ms (US-405).
var latencyBuckets = []float64{0.01, 0.025, 0.05, 0.1, 0.2, 0.3, 0.5, 0.75, 1, 2, 5, 10, 30, 60}

// Metrics are the ingestor's Prometheus series, read by the edge dashboard (US-405).
type Metrics struct {
	Received   prometheus.Counter
	Forwarded  prometheus.Counter
	Duplicates prometheus.Counter
	Invalid    prometheus.Counter
	Retries    prometheus.Counter
	Connected  prometheus.Gauge

	e2e           *prometheus.HistogramVec
	clockSkew     *prometheus.CounterVec
	dropped       *prometheus.GaugeVec
	buffered      *prometheus.GaugeVec
	restarts      *prometheus.GaugeVec
	uptime        *prometheus.GaugeVec
	reconnections *prometheus.CounterVec
	delivered     *prometheus.CounterVec
	boots         *prometheus.CounterVec
}

// NewMetrics registers the ingestor's metrics in reg.
func NewMetrics(reg prometheus.Registerer) *Metrics {
	counter := func(name, help string) prometheus.Counter {
		return prometheus.NewCounter(prometheus.CounterOpts{Name: name, Help: help})
	}
	device := []string{"node_id"}
	m := &Metrics{
		Received:   counter("eop_edge_messages_received_total", "Telemetry messages received from the broker."),
		Forwarded:  counter("eop_edge_messages_forwarded_total", "Telemetry messages acknowledged by eop-server."),
		Duplicates: counter("eop_edge_messages_duplicate_total", "Telemetry messages already delivered: acknowledged, not forwarded (US-404 AC3)."),
		Invalid:    counter("eop_edge_messages_invalid_total", "Messages that break the edge protocol or that eop-server rejected for good."),
		Retries:    counter("eop_edge_forward_retries_total", "Delivery attempts to eop-server that failed and were retried (US-404 AC5)."),
		Connected: prometheus.NewGauge(prometheus.GaugeOpts{Name: "eop_edge_broker_connected",
			Help: "1 while the ingestor is connected to the MQTT broker."}),
		e2e: prometheus.NewHistogramVec(prometheus.HistogramOpts{Name: "eop_edge_e2e_latency_seconds",
			Help:    "Device sampling to eop-server acknowledgement, per device (ADR-028 §3).",
			Buckets: latencyBuckets}, device),
		clockSkew: prometheus.NewCounterVec(prometheus.CounterOpts{Name: "eop_edge_clock_skew_total",
			Help: "Samples acknowledged before their device timestamp: clocks disagree, not recorded as latency."}, device),
		dropped: prometheus.NewGaugeVec(prometheus.GaugeOpts{Name: "eop_edge_device_dropped",
			Help: "Samples the device discarded because its buffer was full, since boot (US-402 AC8)."}, device),
		buffered: prometheus.NewGaugeVec(prometheus.GaugeOpts{Name: "eop_edge_device_buffered",
			Help: "Samples waiting in the device buffer at its last health report."}, device),
		restarts: prometheus.NewGaugeVec(prometheus.GaugeOpts{Name: "eop_edge_device_thread_restarts",
			Help: "Watchdog restarts per device thread, since boot (US-403 AC2)."}, []string{"node_id", "thread"}),
		uptime: prometheus.NewGaugeVec(prometheus.GaugeOpts{Name: "eop_edge_device_uptime_seconds",
			Help: "Device uptime at its last health report."}, device),
		reconnections: prometheus.NewCounterVec(prometheus.CounterOpts{Name: "eop_edge_device_reconnections_total",
			Help: "RECONNECTED events per device (US-402 AC6)."}, device),
		delivered: prometheus.NewCounterVec(prometheus.CounterOpts{Name: "eop_edge_device_buffered_delivered_total",
			Help: "Buffered samples devices reported delivered after reconnecting."}, device),
		boots: prometheus.NewCounterVec(prometheus.CounterOpts{Name: "eop_edge_device_boots_total",
			Help: "BOOT events per device."}, device),
	}
	reg.MustRegister(m.Received, m.Forwarded, m.Duplicates, m.Invalid, m.Retries, m.Connected,
		m.e2e, m.clockSkew, m.dropped, m.buffered, m.restarts, m.uptime, m.reconnections, m.delivered, m.boots)
	return m
}

func (m *Metrics) latency(nodeID string, d time.Duration) {
	if d < 0 {
		m.clockSkew.WithLabelValues(nodeID).Inc()
		return
	}
	m.e2e.WithLabelValues(nodeID).Observe(d.Seconds())
}

func (m *Metrics) event(e edge.Event) {
	switch e.Event {
	case "RECONNECTED":
		m.reconnections.WithLabelValues(e.NodeID).Inc()
		m.delivered.WithLabelValues(e.NodeID).Add(float64(*e.Delivered))
	case "BOOT":
		m.boots.WithLabelValues(e.NodeID).Inc()
	}
}

func (m *Metrics) health(h edge.Health) {
	m.dropped.WithLabelValues(h.NodeID).Set(float64(h.Dropped))
	m.buffered.WithLabelValues(h.NodeID).Set(float64(h.Buffered))
	m.uptime.WithLabelValues(h.NodeID).Set(float64(h.UptimeS))
	for thread, count := range h.Restarts {
		m.restarts.WithLabelValues(h.NodeID, thread).Set(float64(count))
	}
}
