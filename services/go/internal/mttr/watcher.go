package mttr

import (
	"context"
	"log/slog"
	"time"

	"github.com/prometheus/client_golang/prometheus"
)

// Buckets of both histograms: 200 ms is the target (ADR-027 §4), seconds are the replacement.
var durationBuckets = []float64{0.025, 0.05, 0.1, 0.15, 0.2, 0.3, 0.5, 1, 2, 3, 5, 10, 30}

// Metrics are what the Grafana MTTR panel reads.
type Metrics struct {
	durations *prometheus.HistogramVec
	last      *prometheus.GaugeVec
	failures  prometheus.Counter
	ready     prometheus.Gauge
}

// NewMetrics registers the watcher's metrics in `reg`.
func NewMetrics(reg prometheus.Registerer) *Metrics {
	m := &Metrics{
		durations: prometheus.NewHistogramVec(prometheus.HistogramOpts{
			Name:    "eop_mttr_seconds",
			Help:    "Duration of each eop-server Pod failure, by kind: service_restoration (MTTR, target < 200 ms) or replica_replacement (pod failure to replacement Ready).",
			Buckets: durationBuckets,
		}, []string{"kind"}),
		last: prometheus.NewGaugeVec(prometheus.GaugeOpts{
			Name: "eop_mttr_last_seconds",
			Help: "Duration of the last eop-server Pod failure, by kind.",
		}, []string{"kind"}),
		failures: prometheus.NewCounter(prometheus.CounterOpts{
			Name: "eop_mttr_pod_failures_total",
			Help: "eop-server Pods that failed (deleted, crashed or no longer ready).",
		}),
		ready: prometheus.NewGauge(prometheus.GaugeOpts{
			Name: "eop_server_routable_replicas",
			Help: "eop-server Pods the Service routes to now.",
		}),
	}
	reg.MustRegister(m.durations, m.last, m.failures, m.ready)
	return m
}

func (m *Metrics) record(ms []Measurement) {
	for _, measurement := range ms {
		seconds := measurement.Duration.Seconds()
		m.durations.WithLabelValues(string(measurement.Kind)).Observe(seconds)
		m.last.WithLabelValues(string(measurement.Kind)).Set(seconds)
	}
}

// Watcher feeds a Tracker from the Kubernetes API and exports what it measures.
type Watcher struct {
	Kube        *Kube
	PodSelector string // app=eop-server
	Service     string // eop-server
	Metrics     *Metrics
	Logger      *slog.Logger
	Now         func() time.Time
}

// Run watches until ctx ends.
func (w *Watcher) Run(ctx context.Context) error {
	pods := make(chan Event)
	slices := make(chan Event)
	ns := w.Kube.Namespace
	go func() {
		_ = w.Kube.Watch(ctx, "/api/v1/namespaces/"+ns+"/pods", w.PodSelector, pods)
	}()
	go func() {
		_ = w.Kube.Watch(ctx, "/apis/discovery.k8s.io/v1/namespaces/"+ns+"/endpointslices",
			"kubernetes.io/service-name="+w.Service, slices)
	}()

	tracker := NewTracker()
	bySlice := map[string]map[string]bool{}
	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case ev := <-pods:
			w.handlePod(tracker, ev)
		case ev := <-slices:
			w.handleSlice(tracker, bySlice, ev)
		}
	}
}

func (w *Watcher) handlePod(tracker *Tracker, ev Event) {
	at := w.Now()
	state, err := podState(ev.Object, ev.Type == "DELETED")
	if err != nil {
		w.Logger.Warn("unreadable pod event", "error", err)
		return
	}
	before := tracker.Failures
	w.publish(tracker.OnPod(state, at))
	if tracker.Failures > before {
		w.Metrics.failures.Inc()
		w.Logger.Warn("eop-server pod failed", "pod", state.Name, "deleting", state.Deleting, "ready", state.Ready)
	}
}

func (w *Watcher) handleSlice(tracker *Tracker, bySlice map[string]map[string]bool, ev Event) {
	at := w.Now()
	name, routable, err := sliceRoutable(ev.Object)
	if err != nil {
		w.Logger.Warn("unreadable endpointslice event", "error", err)
		return
	}
	if ev.Type == "DELETED" {
		delete(bySlice, name)
	} else {
		bySlice[name] = routable
	}
	merged := map[string]bool{}
	for _, slice := range bySlice {
		for pod, ready := range slice {
			merged[pod] = merged[pod] || ready
		}
	}
	w.publish(tracker.OnEndpoints(merged, at))
	w.Metrics.ready.Set(float64(tracker.ReadyReplicas()))
}

func (w *Watcher) publish(ms []Measurement) {
	w.Metrics.record(ms)
	for _, m := range ms {
		w.Logger.Info("eop-server failure measured", "kind", string(m.Kind), "pod", m.Pod,
			"duration_ms", float64(m.Duration.Microseconds())/1000)
	}
}
