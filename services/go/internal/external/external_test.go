package external

import (
	"context"
	"errors"
	"net/http"
	"net/http/httptest"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	sdktrace "go.opentelemetry.io/otel/sdk/trace"
	"go.opentelemetry.io/otel/sdk/trace/tracetest"

	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/breaker"
)

// fakeUpstream answers according to mode, which tests change at will.
type fakeUpstream struct {
	mode  atomic.Value // "ok", "500", "404", "400", "slow", "notjson"
	calls atomic.Int32
}

func (f *fakeUpstream) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	f.calls.Add(1)
	switch f.mode.Load().(string) {
	case "500":
		w.WriteHeader(http.StatusInternalServerError)
	case "404":
		w.WriteHeader(http.StatusNotFound)
	case "400":
		w.WriteHeader(http.StatusBadRequest)
	case "slow":
		select {
		case <-time.After(time.Second):
		case <-r.Context().Done():
		}
	case "notjson":
		_, _ = w.Write([]byte("<html>"))
	default:
		_, _ = w.Write([]byte(`{"path":"` + r.URL.Path + `"}`))
	}
}

type events struct {
	mu          sync.Mutex
	transitions []string
	fallbacks   []string
}

func fastSettings() breaker.ServiceSettings {
	s := breaker.Defaults()
	s.Breaker.FailureThreshold = 3
	s.Breaker.ResetTimeout = 100 * time.Millisecond
	s.CallTimeout = 200 * time.Millisecond
	return s
}

func newService(t *testing.T, mode string) (*Service, *fakeUpstream, *events) {
	t.Helper()
	up := &fakeUpstream{}
	up.mode.Store(mode)
	srv := httptest.NewServer(up)
	t.Cleanup(srv.Close)
	ev := &events{}
	s, err := New("ai", srv.URL, fastSettings(), 2, Hooks{
		OnTransition: func(tr breaker.Transition) {
			ev.mu.Lock()
			ev.transitions = append(ev.transitions, tr.From.String()+"->"+tr.To.String())
			ev.mu.Unlock()
		},
		OnFallback: func(_, kind string) {
			ev.mu.Lock()
			ev.fallbacks = append(ev.fallbacks, kind)
			ev.mu.Unlock()
		},
	})
	if err != nil {
		t.Fatal(err)
	}
	return s, up, ev
}

func TestFreshAnswerIsCachedAndServedStaleWhenTheServiceFails(t *testing.T) {
	s, up, ev := newService(t, "ok")
	resp, err := s.Get(context.Background(), "/v1/insights/n1", "n1")
	if err != nil || resp.Stale || string(resp.Body) != `{"path":"/v1/insights/n1"}` {
		t.Fatalf("fresh = %+v, %v", resp, err)
	}
	up.mode.Store("500")
	resp, err = s.Get(context.Background(), "/v1/insights/n1", "n1")
	if err != nil || !resp.Stale || resp.StaleSince.IsZero() || string(resp.Body) != `{"path":"/v1/insights/n1"}` {
		t.Fatalf("stale = %+v, %v", resp, err)
	}
	if len(ev.fallbacks) != 1 || ev.fallbacks[0] != FallbackStale {
		t.Fatalf("fallbacks = %v", ev.fallbacks)
	}
}

func TestNoCacheGivesDegradedWithRetryAfter(t *testing.T) {
	s, _, ev := newService(t, "500")
	_, err := s.Get(context.Background(), "/v1/insights/n9", "n9")
	var degraded *DegradedError
	if !errors.As(err, &degraded) || degraded.RetryAfter < time.Second || degraded.Service != "ai" || degraded.Error() == "" || errors.Unwrap(degraded) == nil {
		t.Fatalf("error = %v", err)
	}
	if ev.fallbacks[0] != FallbackDegraded {
		t.Fatalf("fallbacks = %v", ev.fallbacks)
	}
}

// TestCircuitCycle is US-303 AC7 at unit level: failures open the circuit, the open circuit
// stops calling the service, and a recovered service closes it after the reset timeout.
func TestCircuitCycle(t *testing.T) {
	s, up, ev := newService(t, "ok")
	_, _ = s.Get(context.Background(), "/v1/insights/n1", "n1") // fills the cache
	up.mode.Store("500")
	for i := 0; i < 3; i++ {
		_, _ = s.Get(context.Background(), "/v1/insights/n1", "n1")
	}
	if s.Breaker().State() != breaker.Open {
		t.Fatalf("state = %v after 3 failures", s.Breaker().State())
	}
	callsWhenOpened := up.calls.Load()
	resp, err := s.Get(context.Background(), "/v1/insights/n1", "n1")
	if err != nil || !resp.Stale || up.calls.Load() != callsWhenOpened {
		t.Fatal("an open circuit must answer from the cache without calling the service")
	}

	up.mode.Store("ok")
	time.Sleep(120 * time.Millisecond) // reset timeout
	resp, err = s.Get(context.Background(), "/v1/insights/n1", "n1")
	if err != nil || resp.Stale || s.Breaker().State() != breaker.Closed {
		t.Fatalf("after recovery: %+v %v state=%v", resp, err, s.Breaker().State())
	}
	want := []string{"closed->open", "open->half_open", "half_open->closed"}
	if len(ev.transitions) != 3 || ev.transitions[0] != want[0] || ev.transitions[2] != want[2] {
		t.Fatalf("transitions = %v", ev.transitions)
	}
}

func TestFailureKinds(t *testing.T) {
	tests := map[string]struct {
		mode   string
		check  func(error) bool
		counts bool // counts as a breaker failure
	}{
		"timeout":  {"slow", func(err error) bool { var d *DegradedError; return errors.As(err, &d) }, true},
		"not json": {"notjson", func(err error) bool { var d *DegradedError; return errors.As(err, &d) }, true},
		"404":      {"404", func(err error) bool { return errors.Is(err, ErrNotFound) }, false},
		"400":      {"400", func(err error) bool { var r *RejectedError; return errors.As(err, &r) && r.Error() != "" }, false},
	}
	for name, tt := range tests {
		t.Run(name, func(t *testing.T) {
			s, _, _ := newService(t, tt.mode)
			for i := 0; i < 3; i++ {
				if _, err := s.Get(context.Background(), "/x", "k"); !tt.check(err) {
					t.Fatalf("error = %v", err)
				}
			}
			if opened := s.Breaker().State() == breaker.Open; opened != tt.counts {
				t.Fatalf("circuit open = %v, want %v", opened, tt.counts)
			}
		})
	}
}

func TestUnreachableServiceCountsAsFailure(t *testing.T) {
	s, err := New("corporate", "http://127.0.0.1:1", fastSettings(), 2, Hooks{})
	if err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 3; i++ {
		_, _ = s.Get(context.Background(), "/x", "k")
	}
	if s.Breaker().State() != breaker.Open || s.Name() != "corporate" {
		t.Fatal("a refused connection must count as a failure")
	}
}

func TestCallerCancellationDoesNotCount(t *testing.T) {
	s, _, _ := newService(t, "slow")
	for i := 0; i < 5; i++ {
		ctx, cancel := context.WithCancel(context.Background())
		go func() { time.Sleep(10 * time.Millisecond); cancel() }()
		if _, err := s.Get(ctx, "/x", "k"); !errors.Is(err, context.Canceled) {
			t.Fatalf("error = %v", err)
		}
	}
	if s.Breaker().State() != breaker.Closed {
		t.Fatal("clients that hang up must not open the circuit")
	}
}

func TestApplyChangesTimeoutAndThresholds(t *testing.T) {
	s, _, _ := newService(t, "ok")
	next := fastSettings()
	next.CallTimeout = 5 * time.Second
	next.Breaker.FailureThreshold = 1
	if err := s.Apply(next); err != nil {
		t.Fatal(err)
	}
	if s.Breaker().Config().FailureThreshold != 1 || time.Duration(s.timeout.Load()) != 5*time.Second {
		t.Fatal("Apply did not take effect")
	}
	bad := next
	bad.CallTimeout = 0
	if s.Apply(bad) == nil {
		t.Fatal("zero timeout accepted")
	}
	bad = next
	bad.Breaker.MinRequests = 0
	if s.Apply(bad) == nil {
		t.Fatal("invalid breaker config accepted")
	}
}

func TestStateChangeIsASpanEvent(t *testing.T) {
	exporter := tracetest.NewInMemoryExporter()
	provider := sdktrace.NewTracerProvider(sdktrace.WithSyncer(exporter))
	defer func() { _ = provider.Shutdown(context.Background()) }()

	s, _, _ := newService(t, "500")
	for i := 0; i < 3; i++ {
		ctx, span := provider.Tracer("test").Start(context.Background(), "request")
		_, _ = s.Get(ctx, "/x", "k")
		span.End()
	}
	spans := exporter.GetSpans()
	last := spans[len(spans)-1]
	if len(last.Events) != 1 || last.Events[0].Name != "circuit_breaker.state_change" {
		t.Fatalf("events on the request that opened the circuit = %v", last.Events)
	}
	if len(spans[0].Events) != 0 {
		t.Fatal("requests that did not change the state must not carry the event")
	}
}

func TestLRUIsBounded(t *testing.T) {
	c := newLRU(2)
	now := time.Now()
	c.put("a", []byte(`{}`), now)
	c.put("b", []byte(`{}`), now)
	c.put("a", []byte(`{"v":2}`), now) // refresh a
	c.put("c", []byte(`{}`), now)      // evicts b, the least recently used
	if _, _, ok := c.get("b"); ok {
		t.Fatal("b should have been evicted")
	}
	if body, _, ok := c.get("a"); !ok || string(body) != `{"v":2}` {
		t.Fatal("a should hold its latest value")
	}
	if _, err := New("x", "http://h", fastSettings(), 0, Hooks{}); err == nil {
		t.Fatal("cache size 0 accepted")
	}
	bad := fastSettings()
	bad.Breaker.Window = 0
	if _, err := New("x", "http://h", bad, 1, Hooks{}); err == nil {
		t.Fatal("invalid breaker config accepted")
	}
}

func TestInvalidURL(t *testing.T) {
	s, err := New("x", "http://bad host", fastSettings(), 1, Hooks{})
	if err != nil {
		t.Fatal(err)
	}
	var degraded *DegradedError
	if _, err := s.Get(context.Background(), "/x", "k"); !errors.As(err, &degraded) {
		t.Fatalf("error = %v", err)
	}
}
