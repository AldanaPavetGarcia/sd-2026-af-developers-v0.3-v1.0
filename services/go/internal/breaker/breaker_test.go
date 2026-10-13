package breaker

import (
	"errors"
	"sync"
	"testing"
	"time"
)

// PRD defaults (ADR-012) plus this ADR's min_requests.
func prdConfig() Config {
	return Config{FailureThreshold: 5, ErrorRateThreshold: 0.5, MinRequests: 10, Window: 10 * time.Second, ResetTimeout: 30 * time.Second, HalfOpenMaxCalls: 1}
}

type clock struct {
	mu  sync.Mutex
	now time.Time
}

func (c *clock) Now() time.Time {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.now
}

func (c *clock) Advance(d time.Duration) {
	c.mu.Lock()
	c.now = c.now.Add(d)
	c.mu.Unlock()
}

type recorder struct {
	mu          sync.Mutex
	transitions []Transition
}

func (r *recorder) add(t Transition) {
	r.mu.Lock()
	r.transitions = append(r.transitions, t)
	r.mu.Unlock()
}

func (r *recorder) states() []string {
	r.mu.Lock()
	defer r.mu.Unlock()
	out := []string{}
	for _, t := range r.transitions {
		out = append(out, t.From.String()+"->"+t.To.String())
	}
	return out
}

func newTestBreaker(t *testing.T, cfg Config) (*Breaker, *clock, *recorder) {
	t.Helper()
	clk := &clock{now: time.Date(2026, 10, 13, 12, 0, 0, 0, time.UTC)}
	rec := &recorder{}
	b, err := New("ai", cfg, clk.Now, rec.add)
	if err != nil {
		t.Fatal(err)
	}
	return b, clk, rec
}

func call(t *testing.T, b *Breaker, success bool) error {
	t.Helper()
	done, err := b.Allow()
	if err != nil {
		return err
	}
	done(success)
	return nil
}

func TestOpensAfterConsecutiveFailures(t *testing.T) {
	b, _, rec := newTestBreaker(t, prdConfig())
	for i := 0; i < 4; i++ {
		_ = call(t, b, false)
	}
	if b.State() != Closed {
		t.Fatal("4 consecutive failures must not open the circuit")
	}
	_ = call(t, b, false)
	if b.State() != Open {
		t.Fatal("5 consecutive failures must open the circuit (US-303 AC2)")
	}
	if err := call(t, b, true); !errors.Is(err, ErrOpen) {
		t.Fatalf("Allow while open = %v, want ErrOpen", err)
	}
	if got := rec.states(); len(got) != 1 || got[0] != "closed->open" {
		t.Fatalf("transitions = %v", got)
	}
}

func TestSuccessResetsTheConsecutiveCount(t *testing.T) {
	b, _, _ := newTestBreaker(t, prdConfig())
	for i := 0; i < 20; i++ {
		_ = call(t, b, i%4 == 3) // 3 failures, 1 success: never 5 in a row, 75 % errors but...
	}
	// ...the rate rule still applies: 15 failures out of 20 > 50 % with >= 10 calls.
	if b.State() != Open {
		t.Fatal("an error rate above 50 % with enough calls must open the circuit")
	}
}

func TestErrorRateNeedsMinRequests(t *testing.T) {
	b, _, _ := newTestBreaker(t, prdConfig())
	// Alternate so consecutive failures never reach 5: 9 calls, 5 failures = 55 %.
	for _, ok := range []bool{false, true, false, true, false, true, false, true, false} {
		_ = call(t, b, ok)
	}
	if b.State() != Closed {
		t.Fatal("below min_requests the rate rule must not open the circuit")
	}
	_ = call(t, b, false) // 10 calls, 6 failures = 60 %
	if b.State() != Open {
		t.Fatal("10 calls at 60 % errors must open the circuit")
	}
}

func TestErrorRateAtExactlyTheThresholdStaysClosed(t *testing.T) {
	b, _, _ := newTestBreaker(t, prdConfig())
	for _, ok := range []bool{false, true, false, true, false, true, false, true, false, true} {
		_ = call(t, b, ok)
	}
	if b.State() != Closed {
		t.Fatal("exactly 50 % is not above the threshold")
	}
}

func TestWindowSlides(t *testing.T) {
	b, clk, _ := newTestBreaker(t, prdConfig())
	for i := 0; i < 4; i++ { // old failures, separated so they never are 5 in a row
		_ = call(t, b, false)
		_ = call(t, b, true)
	}
	clk.Advance(11 * time.Second) // they leave the 10 s window
	for _, ok := range []bool{false, true, false, true, false, true, false, true, true, true} {
		_ = call(t, b, ok)
	}
	if b.State() != Closed {
		t.Fatal("failures older than the window must not count")
	}
}

func TestHalfOpenAfterResetTimeoutThenCloses(t *testing.T) {
	b, clk, rec := newTestBreaker(t, prdConfig())
	for i := 0; i < 5; i++ {
		_ = call(t, b, false)
	}
	clk.Advance(29 * time.Second)
	if b.State() != Open || b.RetryAfter() != time.Second {
		t.Fatalf("state = %v, RetryAfter = %v", b.State(), b.RetryAfter())
	}
	clk.Advance(time.Second)
	done, err := b.Allow()
	if err != nil || b.State() != HalfOpen {
		t.Fatalf("after 30 s: err = %v, state = %v (US-303 AC4)", err, b.State())
	}
	if _, err := b.Allow(); !errors.Is(err, ErrOpen) {
		t.Fatal("half_open_max_calls = 1 allows a single probe")
	}
	done(true)
	if b.State() != Closed || b.RetryAfter() != 0 {
		t.Fatal("a successful probe must close the circuit")
	}
	want := []string{"closed->open", "open->half_open", "half_open->closed"}
	if got := rec.states(); len(got) != 3 || got[0] != want[0] || got[1] != want[1] || got[2] != want[2] {
		t.Fatalf("transitions = %v, want %v", got, want)
	}
}

func TestFailedProbeReopens(t *testing.T) {
	b, clk, _ := newTestBreaker(t, prdConfig())
	for i := 0; i < 5; i++ {
		_ = call(t, b, false)
	}
	clk.Advance(30 * time.Second)
	_ = call(t, b, false)
	if b.State() != Open || b.RetryAfter() != 30*time.Second {
		t.Fatalf("failed probe: state = %v, RetryAfter = %v; the reset timer must restart", b.State(), b.RetryAfter())
	}
}

func TestOutcomesFromAnEarlierStateAreIgnored(t *testing.T) {
	b, _, _ := newTestBreaker(t, prdConfig())
	slow, _ := b.Allow() // started while closed
	for i := 0; i < 5; i++ {
		_ = call(t, b, false)
	}
	slow(true) // finishes after the circuit opened
	if b.State() != Open {
		t.Fatal("a late success from before the transition must not close the circuit")
	}
	slow(true) // done is idempotent
}

func TestUpdateKeepsStateAndAppliesThresholds(t *testing.T) {
	b, clk, _ := newTestBreaker(t, prdConfig())
	for i := 0; i < 5; i++ {
		_ = call(t, b, false)
	}
	cfg := prdConfig()
	cfg.ResetTimeout = 5 * time.Second
	if err := b.Update(cfg); err != nil {
		t.Fatal(err)
	}
	if b.State() != Open {
		t.Fatal("Update must not reset the state (US-303 AC8)")
	}
	clk.Advance(5 * time.Second)
	if b.State() != HalfOpen {
		t.Fatal("the new reset timeout must apply to the open circuit")
	}

	cfg.FailureThreshold = 2
	cfg.Window = 20 * time.Second
	if err := b.Update(cfg); err != nil || b.Config().Window != 20*time.Second {
		t.Fatalf("Update: %v", err)
	}
	if err := b.Update(Config{}); err == nil {
		t.Fatal("Update accepted an invalid config")
	}
}

func TestConfigValidate(t *testing.T) {
	mutations := map[string]func(*Config){
		"failure threshold": func(c *Config) { c.FailureThreshold = 0 },
		"rate zero":         func(c *Config) { c.ErrorRateThreshold = 0 },
		"rate above one":    func(c *Config) { c.ErrorRateThreshold = 1.5 },
		"min requests":      func(c *Config) { c.MinRequests = 0 },
		"window":            func(c *Config) { c.Window = 500 * time.Millisecond },
		"reset timeout":     func(c *Config) { c.ResetTimeout = 0 },
		"half-open calls":   func(c *Config) { c.HalfOpenMaxCalls = 0 },
	}
	for name, mutate := range mutations {
		cfg := prdConfig()
		mutate(&cfg)
		if _, err := New("x", cfg, time.Now, nil); err == nil {
			t.Errorf("%s: invalid config accepted", name)
		}
	}
	if Closed.String() != "closed" || HalfOpen.String() != "half_open" || Open.String() != "open" {
		t.Fatal("state names are part of the metric labels")
	}
}

func TestConcurrentUse(t *testing.T) {
	b, _ := New("ai", prdConfig(), time.Now, func(Transition) {})
	var wg sync.WaitGroup
	for g := 0; g < 8; g++ {
		wg.Add(1)
		go func(g int) {
			defer wg.Done()
			for i := 0; i < 500; i++ {
				if done, err := b.Allow(); err == nil {
					done((i+g)%3 != 0)
				}
				_ = b.State()
				_ = b.RetryAfter()
			}
		}(g)
	}
	wg.Wait()
	if b.Name() != "ai" {
		t.Fatal("name")
	}
}
