package ingest

import (
	"context"
	"errors"
	"io"
	"log/slog"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/testutil"
	sdktrace "go.opentelemetry.io/otel/sdk/trace"
	"go.opentelemetry.io/otel/sdk/trace/tracetest"

	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/edge"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/eopclient"
)

const prefix = "eop/bunker"

var sampled = time.Date(2026, 11, 2, 14, 3, 7, 120e6, time.UTC)

func telemetryPayload(nodeID string, messageID int) []byte {
	return []byte(`{"node_id":"` + nodeID + `","bunker_name":"Vault-101","timestamp":"2026-11-02T14:03:07.120Z","temperature":21.4,"status":"OK","message_id":` +
		itoa(messageID) + `,"boot_id":"9f3c27a1"}`)
}

func itoa(n int) string { return strconv.Itoa(n) }

// fakeForwarder answers from a script of errors, then nil.
type fakeForwarder struct {
	mu      sync.Mutex
	errs    []error
	samples []eopclient.Sample
	ctxs    []context.Context
}

func (f *fakeForwarder) Telemetry(ctx context.Context, s eopclient.Sample) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.samples = append(f.samples, s)
	f.ctxs = append(f.ctxs, ctx)
	if len(f.errs) > 0 {
		err := f.errs[0]
		f.errs = f.errs[1:]
		return err
	}
	return nil
}

type fakeDedup struct {
	keys    map[string]time.Duration
	seenErr error
	markErr error
}

func (d *fakeDedup) Seen(_ context.Context, key string) (bool, error) {
	_, ok := d.keys[key]
	return ok, d.seenErr
}

func (d *fakeDedup) Mark(_ context.Context, key string, ttl time.Duration) error {
	if d.markErr != nil {
		return d.markErr
	}
	d.keys[key] = ttl
	return nil
}

type rig struct {
	p      *Processor
	fwd    *fakeForwarder
	dedup  *fakeDedup
	spans  *tracetest.SpanRecorder
	sleeps []time.Duration
	now    time.Time
}

func newRig(t *testing.T) *rig {
	t.Helper()
	r := &rig{fwd: &fakeForwarder{}, dedup: &fakeDedup{keys: map[string]time.Duration{}}, spans: tracetest.NewSpanRecorder(),
		now: sampled.Add(80 * time.Millisecond)}
	tp := sdktrace.NewTracerProvider(sdktrace.WithSpanProcessor(r.spans))
	r.p = &Processor{
		Prefix: prefix, Forward: r.fwd, Dedup: r.dedup, DedupTTL: 5 * time.Minute,
		Retry:  Backoff{Initial: 100 * time.Millisecond, Max: 400 * time.Millisecond},
		Window: edge.NewWindow(time.Minute), Metrics: NewMetrics(prometheus.NewRegistry()),
		Tracer: tp.Tracer("test"), Logger: slog.New(slog.NewTextHandler(io.Discard, nil)),
		Now:   func() time.Time { return r.now },
		Sleep: func(ctx context.Context, d time.Duration) error { r.sleeps = append(r.sleeps, d); return ctx.Err() },
	}
	return r
}

func (r *rig) handle(topic string, payload []byte) Outcome {
	return r.p.Handle(context.Background(), Message{Topic: topic, Payload: payload, ReceivedAt: sampled.Add(20 * time.Millisecond)})
}

func TestForwardsTelemetryAndMarksItAfterTheAck(t *testing.T) {
	r := newRig(t)
	if got := r.handle("eop/bunker/vault-101/telemetry", telemetryPayload("vault-101", 7)); got != Forwarded {
		t.Fatalf("outcome %s", got)
	}
	s := r.fwd.samples[0]
	if s.NodeID != "vault-101" || s.MessageID != 7 || s.BootID != "9f3c27a1" || !s.SampledAt.Equal(sampled) || s.Temperature != 21.4 {
		t.Fatalf("sample %+v", s)
	}
	if ttl := r.dedup.keys["edge:dedup:vault-101:9f3c27a1:7"]; ttl != 5*time.Minute {
		t.Fatalf("dedup keys %v", r.dedup.keys)
	}
	if testutil.ToFloat64(r.p.Metrics.Forwarded) != 1 || testutil.ToFloat64(r.p.Metrics.Received) != 1 {
		t.Fatal("counters")
	}
	if n := testutil.CollectAndCount(r.p.Metrics.e2e); n != 1 {
		t.Fatalf("latency series %d", n)
	}
	// Spans: edge.message from sampling, edge.ingest from receipt, ingest is the forwarded context.
	ended := r.spans.Ended()
	if len(ended) != 2 || ended[0].Name() != "edge.ingest" || ended[1].Name() != "edge.message" {
		t.Fatalf("spans %v", ended)
	}
	if !ended[1].StartTime().Equal(sampled) || !ended[0].StartTime().Equal(sampled.Add(20*time.Millisecond)) {
		t.Fatal("span start times")
	}
	if ended[0].Parent().SpanID() != ended[1].SpanContext().SpanID() {
		t.Fatal("edge.ingest must be a child of edge.message")
	}
	// US-405 AC1: the three timestamps and the latency on the message's trace.
	attrs := map[string]string{}
	for _, kv := range ended[1].Attributes() {
		attrs[string(kv.Key)] = kv.Value.String()
	}
	want := map[string]string{
		"edge.sampled_at":     "2026-11-02T14:03:07.12Z",
		"edge.received_at":    "2026-11-02T14:03:07.14Z",
		"edge.delivered_at":   "2026-11-02T14:03:07.2Z",
		"edge.e2e_latency_ms": "80",
	}
	for key, value := range want {
		if attrs[key] != value {
			t.Fatalf("%s = %q, want %q (all: %v)", key, attrs[key], value, attrs)
		}
	}
	if got := testutil.ToFloat64(r.p.Metrics.lastSeen.WithLabelValues("vault-101")); got != float64(sampled.Add(20*time.Millisecond).UnixMilli())/1000 {
		t.Fatalf("last seen %v", got)
	}
	snap := r.p.Window.Snapshot(sampled.Add(time.Second), edge.StateConnected, 10*time.Second)
	if snap.Received != 1 || snap.Forwarded != 1 || len(snap.Devices) != 1 {
		t.Fatalf("window %+v", snap)
	}
}

func TestDuplicateIsAcknowledgedNotForwarded(t *testing.T) {
	r := newRig(t)
	r.handle("eop/bunker/vault-101/telemetry", telemetryPayload("vault-101", 7))
	if got := r.handle("eop/bunker/vault-101/telemetry", telemetryPayload("vault-101", 7)); got != Duplicate {
		t.Fatalf("outcome %s", got)
	}
	if len(r.fwd.samples) != 1 || testutil.ToFloat64(r.p.Metrics.Duplicates) != 1 {
		t.Fatalf("forwarded %d", len(r.fwd.samples))
	}
	// Another boot of the same device restarts message ids: not a duplicate.
	other := strings.Replace(string(telemetryPayload("vault-101", 7)), "9f3c27a1", "00000001", 1)
	if got := r.handle("eop/bunker/vault-101/telemetry", []byte(other)); got != Forwarded {
		t.Fatalf("new boot: %s", got)
	}
}

func TestRetriesWithBackoffUntilTheServerTakesIt(t *testing.T) {
	r := newRig(t)
	down := &eopclient.UnavailableError{Err: errors.New("connection refused")}
	busy := &eopclient.ProtocolError{Code: eopclient.CodeServiceUnavailable, Desc: "Node registry unavailable"}
	r.fwd.errs = []error{down, down, busy, down}
	if got := r.handle("eop/bunker/vault-101/telemetry", telemetryPayload("vault-101", 1)); got != Forwarded {
		t.Fatalf("outcome %s", got)
	}
	want := []time.Duration{100 * time.Millisecond, 200 * time.Millisecond, 400 * time.Millisecond, 400 * time.Millisecond}
	if len(r.sleeps) != len(want) {
		t.Fatalf("sleeps %v", r.sleeps)
	}
	for i := range want {
		if r.sleeps[i] != want[i] {
			t.Fatalf("sleeps %v, want %v", r.sleeps, want)
		}
	}
	if testutil.ToFloat64(r.p.Metrics.Retries) != 4 || len(r.fwd.samples) != 5 {
		t.Fatal("retries")
	}
}

func TestStopDuringRetriesLeavesTheMessageToTheBroker(t *testing.T) {
	r := newRig(t)
	r.fwd.errs = []error{&eopclient.UnavailableError{Err: errors.New("down")}}
	ctx, cancel := context.WithCancel(context.Background())
	r.p.Sleep = func(context.Context, time.Duration) error { cancel(); return context.Canceled }
	got := r.p.Handle(ctx, Message{Topic: "eop/bunker/vault-101/telemetry", Payload: telemetryPayload("vault-101", 1), ReceivedAt: sampled})
	if got != Stopped || len(r.dedup.keys) != 0 {
		t.Fatalf("outcome %s, keys %v", got, r.dedup.keys)
	}
	// Cancelled while the forwarder itself fails.
	r2 := newRig(t)
	ctx2, cancel2 := context.WithCancel(context.Background())
	cancel2()
	r2.fwd.errs = []error{context.Canceled}
	if got := r2.p.Handle(ctx2, Message{Topic: "eop/bunker/vault-101/telemetry", Payload: telemetryPayload("vault-101", 1), ReceivedAt: sampled}); got != Stopped {
		t.Fatalf("outcome %s", got)
	}
}

func TestPermanentRejectionIsCountedAsInvalid(t *testing.T) {
	r := newRig(t)
	r.fwd.errs = []error{&eopclient.ProtocolError{Code: eopclient.CodeDuplicate, Desc: "node_id registered over TCP"}}
	if got := r.handle("eop/bunker/vault-101/telemetry", telemetryPayload("vault-101", 1)); got != Invalid {
		t.Fatalf("outcome %s", got)
	}
	if len(r.dedup.keys) != 0 || testutil.ToFloat64(r.p.Metrics.Invalid) != 1 || len(r.sleeps) != 0 {
		t.Fatal("a rejected sample is neither retried nor marked delivered")
	}
}

func TestInvalidMessages(t *testing.T) {
	r := newRig(t)
	for topic, payload := range map[string]string{
		"eop/bunker/vault-101/telemetry": `{`,
		"eop/bunker/vault-101/events":    `{"event":"CRASH"}`,
		"eop/bunker/vault-101/health":    `{}`,
		"eop/bunker/vault-101/other":     `{}`,
		"other/vault-101/telemetry":      `{}`,
	} {
		if got := r.handle(topic, []byte(payload)); got != Invalid {
			t.Errorf("%s: %s", topic, got)
		}
	}
	if testutil.ToFloat64(r.p.Metrics.Invalid) != 5 || len(r.fwd.samples) != 0 {
		t.Fatal("invalid messages must be counted and never forwarded")
	}
}

func TestDedupStoreDownStillForwards(t *testing.T) {
	r := newRig(t)
	r.dedup.seenErr = errors.New("redis down")
	r.dedup.markErr = errors.New("redis down")
	if got := r.handle("eop/bunker/vault-101/telemetry", telemetryPayload("vault-101", 1)); got != Forwarded {
		t.Fatalf("outcome %s", got)
	}
}

func TestEventsAndHealthFeedMetrics(t *testing.T) {
	r := newRig(t)
	ts := `"timestamp":"2026-11-02T14:03:07.120Z"`
	id := `"node_id":"vault-101","boot_id":"9f3c27a1",`
	cases := []string{
		`{` + id + ts + `,"event":"BOOT"}`,
		`{` + id + ts + `,"event":"RECONNECTED","delivered":37}`,
	}
	for _, payload := range cases {
		if got := r.handle("eop/bunker/vault-101/events", []byte(payload)); got != Recorded {
			t.Fatalf("%s: %s", payload, got)
		}
	}
	health := `{` + id + ts + `,"uptime_s":60,"restarts":{"sensor":0,"mqtt":2},"buffered":3,"dropped":15}`
	if got := r.handle("eop/bunker/vault-101/health", []byte(health)); got != Recorded {
		t.Fatalf("health: %s", got)
	}
	m := r.p.Metrics
	if testutil.ToFloat64(m.boots.WithLabelValues("vault-101")) != 1 ||
		testutil.ToFloat64(m.reconnections.WithLabelValues("vault-101")) != 1 ||
		testutil.ToFloat64(m.delivered.WithLabelValues("vault-101")) != 37 ||
		testutil.ToFloat64(m.dropped.WithLabelValues("vault-101")) != 15 ||
		testutil.ToFloat64(m.buffered.WithLabelValues("vault-101")) != 3 ||
		testutil.ToFloat64(m.uptime.WithLabelValues("vault-101")) != 60 ||
		testutil.ToFloat64(m.restarts.WithLabelValues("vault-101", "mqtt")) != 2 {
		t.Fatal("device metrics")
	}
	if len(r.fwd.samples) != 0 {
		t.Fatal("events and health are not forwarded")
	}
	if testutil.ToFloat64(m.lastSeen.WithLabelValues("vault-101")) == 0 {
		t.Fatal("events and health also mark the device as seen")
	}
}

func TestClockSkewIsNotLatency(t *testing.T) {
	r := newRig(t)
	r.now = sampled.Add(-time.Second) // the server acknowledged "before" the device sampled
	r.handle("eop/bunker/vault-101/telemetry", telemetryPayload("vault-101", 1))
	if testutil.ToFloat64(r.p.Metrics.clockSkew.WithLabelValues("vault-101")) != 1 || testutil.CollectAndCount(r.p.Metrics.e2e) != 0 {
		t.Fatal("negative latency must be counted as skew")
	}
}

func TestDefaultSleepHonoursTheContext(t *testing.T) {
	p := &Processor{}
	if err := p.sleep(context.Background(), time.Millisecond); err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if err := p.sleep(ctx, time.Hour); err == nil {
		t.Fatal("cancelled sleep returned nil")
	}
}
