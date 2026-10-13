// Package breaker is a circuit breaker with a sliding error-rate window (ADR-012, US-303).
//
// CLOSED lets calls through and opens after FailureThreshold consecutive failures, or when the
// error rate over Window exceeds ErrorRateThreshold with at least MinRequests calls. OPEN
// rejects calls until ResetTimeout has passed, then HALF-OPEN lets HalfOpenMaxCalls probes
// through: a success closes the circuit, a failure opens it again.
package breaker

import (
	"errors"
	"fmt"
	"math"
	"sync"
	"time"
)

// State of a circuit.
type State int

// States, valued as the eop_gateway_circuit_state gauge reports them.
const (
	Closed   State = 0
	HalfOpen State = 1
	Open     State = 2
)

func (s State) String() string {
	switch s {
	case Closed:
		return "closed"
	case HalfOpen:
		return "half_open"
	default:
		return "open"
	}
}

// bucketWidth is the resolution of the sliding window.
const bucketWidth = time.Second

// ErrOpen is returned by Allow while the circuit rejects calls.
var ErrOpen = errors.New("breaker: circuit open")

// Config holds the thresholds of ADR-012 / US-303 AC2 and AC4.
type Config struct {
	FailureThreshold   int
	ErrorRateThreshold float64
	MinRequests        int
	Window             time.Duration
	ResetTimeout       time.Duration
	HalfOpenMaxCalls   int
}

// Validate reports the first value that cannot work.
func (c Config) Validate() error {
	switch {
	case c.FailureThreshold < 1:
		return errors.New("breaker: failure_threshold must be at least 1")
	case c.ErrorRateThreshold <= 0 || c.ErrorRateThreshold > 1:
		return errors.New("breaker: error_rate_threshold must be in (0, 1]")
	case c.MinRequests < 1:
		return errors.New("breaker: min_requests must be at least 1")
	case c.Window < bucketWidth:
		return fmt.Errorf("breaker: window must be at least %v", bucketWidth)
	case c.ResetTimeout <= 0:
		return errors.New("breaker: reset_timeout must be positive")
	case c.HalfOpenMaxCalls < 1:
		return errors.New("breaker: half_open_max_calls must be at least 1")
	}
	return nil
}

// Transition is reported every time the state changes.
type Transition struct {
	Name     string
	From, To State
	Reason   string
}

type bucket struct {
	second    int64
	successes int
	failures  int
}

// Breaker is safe for concurrent use.
type Breaker struct {
	name         string
	now          func() time.Time
	onTransition func(Transition)

	mu                  sync.Mutex
	cfg                 Config
	state               State
	generation          uint64
	consecutiveFailures int
	buckets             []bucket
	openedAt            time.Time
	probesInFlight      int
}

// New builds a closed breaker. onTransition, if not nil, is called after every state change,
// outside the breaker's lock. now is the clock (time.Now in production).
func New(name string, cfg Config, now func() time.Time, onTransition func(Transition)) (*Breaker, error) {
	if err := cfg.Validate(); err != nil {
		return nil, err
	}
	return &Breaker{
		name: name, now: now, onTransition: onTransition,
		cfg: cfg, buckets: make([]bucket, bucketCount(cfg.Window)),
	}, nil
}

func bucketCount(window time.Duration) int {
	return int(math.Ceil(float64(window) / float64(bucketWidth)))
}

// Name of the protected service.
func (b *Breaker) Name() string { return b.name }

// State returns the current state, moving OPEN to HALF-OPEN if the reset timeout has passed.
func (b *Breaker) State() State {
	b.mu.Lock()
	transitions := b.advance(b.now())
	state := b.state
	b.mu.Unlock()
	b.notify(transitions)
	return state
}

// RetryAfter is how long the circuit stays OPEN, or zero when it accepts calls.
func (b *Breaker) RetryAfter() time.Duration {
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.state != Open {
		return 0
	}
	return max(0, b.cfg.ResetTimeout-b.now().Sub(b.openedAt))
}

// Allow asks to make one call. It returns ErrOpen while the circuit rejects calls; otherwise
// the caller must report the outcome through done exactly once.
func (b *Breaker) Allow() (done func(success bool), err error) {
	b.mu.Lock()
	now := b.now()
	transitions := b.advance(now)
	switch b.state {
	case Open:
		b.mu.Unlock()
		b.notify(transitions)
		return nil, ErrOpen
	case HalfOpen:
		if b.probesInFlight >= b.cfg.HalfOpenMaxCalls {
			b.mu.Unlock()
			b.notify(transitions)
			return nil, ErrOpen
		}
		b.probesInFlight++
	}
	generation := b.generation
	b.mu.Unlock()
	b.notify(transitions)

	var once sync.Once
	return func(success bool) {
		once.Do(func() { b.record(generation, success) })
	}, nil
}

// record applies one outcome. Outcomes of calls started before the last state change are
// ignored: they describe a circuit that no longer exists.
func (b *Breaker) record(generation uint64, success bool) {
	b.mu.Lock()
	var transitions []Transition
	if generation == b.generation {
		now := b.now()
		switch b.state {
		case HalfOpen:
			b.probesInFlight--
			if success {
				transitions = append(transitions, b.moveTo(Closed, now, "probe succeeded"))
			} else {
				transitions = append(transitions, b.moveTo(Open, now, "probe failed"))
			}
		case Closed:
			transitions = b.recordClosed(success, now)
		}
	}
	b.mu.Unlock()
	b.notify(transitions)
}

func (b *Breaker) recordClosed(success bool, now time.Time) []Transition {
	bk := b.bucketFor(now)
	if success {
		bk.successes++
		b.consecutiveFailures = 0
		return nil
	}
	bk.failures++
	b.consecutiveFailures++
	if b.consecutiveFailures >= b.cfg.FailureThreshold {
		return []Transition{b.moveTo(Open, now, fmt.Sprintf("%d consecutive failures", b.consecutiveFailures))}
	}
	successes, failures := b.windowCounts(now)
	total := successes + failures
	if total >= b.cfg.MinRequests && float64(failures)/float64(total) > b.cfg.ErrorRateThreshold {
		return []Transition{b.moveTo(Open, now, fmt.Sprintf("error rate %d/%d over %v", failures, total, b.cfg.Window))}
	}
	return nil
}

func (b *Breaker) bucketFor(now time.Time) *bucket {
	second := now.Unix()
	bk := &b.buckets[int(second%int64(len(b.buckets)))]
	if bk.second != second {
		*bk = bucket{second: second}
	}
	return bk
}

func (b *Breaker) windowCounts(now time.Time) (successes, failures int) {
	oldest := now.Unix() - int64(len(b.buckets)) + 1
	for _, bk := range b.buckets {
		if bk.second >= oldest {
			successes += bk.successes
			failures += bk.failures
		}
	}
	return successes, failures
}

// advance moves OPEN to HALF-OPEN once the reset timeout has passed (US-303 AC4).
func (b *Breaker) advance(now time.Time) []Transition {
	if b.state == Open && now.Sub(b.openedAt) >= b.cfg.ResetTimeout {
		return []Transition{b.moveTo(HalfOpen, now, "reset timeout elapsed")}
	}
	return nil
}

func (b *Breaker) moveTo(to State, now time.Time, reason string) Transition {
	t := Transition{Name: b.name, From: b.state, To: to, Reason: reason}
	b.state = to
	b.generation++
	b.consecutiveFailures = 0
	b.probesInFlight = 0
	for i := range b.buckets {
		b.buckets[i] = bucket{}
	}
	if to == Open {
		b.openedAt = now
	}
	return t
}

func (b *Breaker) notify(transitions []Transition) {
	if b.onTransition == nil {
		return
	}
	for _, t := range transitions {
		b.onTransition(t)
	}
}

// Update applies new thresholds without resetting the state (US-303 AC8). A different window
// size starts a fresh window, since its buckets cannot be resized in place.
func (b *Breaker) Update(cfg Config) error {
	if err := cfg.Validate(); err != nil {
		return err
	}
	b.mu.Lock()
	defer b.mu.Unlock()
	if n := bucketCount(cfg.Window); n != len(b.buckets) {
		b.buckets = make([]bucket, n)
	}
	b.cfg = cfg
	return nil
}

// Config returns the thresholds in use.
func (b *Breaker) Config() Config {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.cfg
}
