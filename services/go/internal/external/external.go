// Package external calls the external AI and corporate services through a circuit breaker,
// with a per-call timeout and a cache of the last good answer as fallback (ADR-012, US-303).
package external

import (
	"container/list"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"sync"
	"sync/atomic"
	"time"

	"go.opentelemetry.io/otel/attribute"
	"go.opentelemetry.io/otel/trace"

	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/breaker"
)

// maxBodyBytes bounds an external answer; both services return small JSON objects.
const maxBodyBytes = 1 << 20

// minRetryAfter avoids telling clients to retry immediately while a circuit is open.
const minRetryAfter = time.Second

// Fallback kinds, as counted in eop_gateway_fallback_responses_total.
const (
	FallbackStale    = "stale"
	FallbackDegraded = "degraded"
)

// ErrNotFound means the external service does not know the requested item.
var ErrNotFound = errors.New("external: not found")

// DegradedError means the service could not be used and no cached answer exists. The API turns
// it into 503 with Retry-After (US-303 AC3, never a 500).
type DegradedError struct {
	Service    string
	RetryAfter time.Duration
	Cause      error
}

func (e *DegradedError) Error() string {
	return fmt.Sprintf("external: %s degraded: %v", e.Service, e.Cause)
}

func (e *DegradedError) Unwrap() error { return e.Cause }

// RejectedError means the service answered with a client error other than 404: the request
// the gateway built was wrong, which is not the service's failure.
type RejectedError struct {
	Service string
	Status  int
}

func (e *RejectedError) Error() string {
	return fmt.Sprintf("external: %s rejected the request with %d", e.Service, e.Status)
}

// Response is an answer, fresh or served from the cache while the service is unusable.
type Response struct {
	Body       json.RawMessage
	Stale      bool
	StaleSince time.Time
}

// Hooks report what the service does; any may be nil.
type Hooks struct {
	OnTransition func(breaker.Transition)
	OnFallback   func(service, kind string)
}

// Service is one external dependency. It is safe for concurrent use.
type Service struct {
	name    string
	baseURL string
	client  *http.Client
	breaker *breaker.Breaker
	timeout atomic.Int64 // nanoseconds; changed by Apply
	cache   *lru
	hooks   Hooks
	now     func() time.Time
}

// New builds a service client. baseURL has no trailing slash.
func New(name, baseURL string, settings breaker.ServiceSettings, cacheSize int, hooks Hooks) (*Service, error) {
	if cacheSize < 1 {
		return nil, errors.New("external: cache size must be at least 1")
	}
	b, err := breaker.New(name, settings.Breaker, time.Now, hooks.OnTransition)
	if err != nil {
		return nil, err
	}
	s := &Service{name: name, baseURL: baseURL, client: &http.Client{}, breaker: b, cache: newLRU(cacheSize), hooks: hooks, now: time.Now}
	s.timeout.Store(int64(settings.CallTimeout))
	return s, nil
}

// Name of the service.
func (s *Service) Name() string { return s.name }

// Breaker exposes the circuit, for health and metrics.
func (s *Service) Breaker() *breaker.Breaker { return s.breaker }

// Apply changes thresholds and timeout without resetting the circuit (US-303 AC8).
func (s *Service) Apply(settings breaker.ServiceSettings) error {
	if settings.CallTimeout <= 0 {
		return errors.New("external: call timeout must be positive")
	}
	if err := s.breaker.Update(settings.Breaker); err != nil {
		return err
	}
	s.timeout.Store(int64(settings.CallTimeout))
	return nil
}

// Get calls path and returns the answer, or the cached answer for cacheKey when the service is
// failing or its circuit is open.
func (s *Service) Get(ctx context.Context, path, cacheKey string) (Response, error) {
	before := s.breaker.State()
	defer func() {
		// The span event goes on the request that caused the change (ADR-012 §5).
		if after := s.breaker.State(); after != before {
			trace.SpanFromContext(ctx).AddEvent("circuit_breaker.state_change", trace.WithAttributes(
				attribute.String("service", s.name), attribute.String("from", before.String()), attribute.String("to", after.String())))
		}
	}()

	done, err := s.breaker.Allow()
	if err != nil {
		return s.fallback(cacheKey, err)
	}
	body, status, err := s.do(ctx, path)
	switch {
	case err != nil && ctx.Err() != nil && !errors.Is(ctx.Err(), context.DeadlineExceeded):
		// The caller went away: not the service's fault.
		done(true)
		return Response{}, ctx.Err()
	case err != nil:
		done(false)
		return s.fallback(cacheKey, err)
	case status >= http.StatusInternalServerError:
		done(false)
		return s.fallback(cacheKey, fmt.Errorf("status %d", status))
	case status == http.StatusNotFound:
		done(true)
		return Response{}, ErrNotFound
	case status >= http.StatusBadRequest:
		done(true)
		return Response{}, &RejectedError{Service: s.name, Status: status}
	}
	if !isJSONObject(body) {
		done(false)
		return s.fallback(cacheKey, errors.New("answer is not a JSON object"))
	}
	done(true)
	s.cache.put(cacheKey, body, s.now())
	return Response{Body: body}, nil
}

func (s *Service) do(ctx context.Context, path string) ([]byte, int, error) {
	ctx, cancel := context.WithTimeout(ctx, time.Duration(s.timeout.Load()))
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, s.baseURL+path, nil)
	if err != nil {
		return nil, 0, err
	}
	req.Header.Set("Accept", "application/json")
	resp, err := s.client.Do(req)
	if err != nil {
		return nil, 0, err
	}
	defer func() { _ = resp.Body.Close() }()
	body, err := io.ReadAll(io.LimitReader(resp.Body, maxBodyBytes))
	return body, resp.StatusCode, err
}

func (s *Service) fallback(cacheKey string, cause error) (Response, error) {
	if body, at, ok := s.cache.get(cacheKey); ok {
		s.report(FallbackStale)
		return Response{Body: body, Stale: true, StaleSince: at}, nil
	}
	s.report(FallbackDegraded)
	return Response{}, &DegradedError{Service: s.name, RetryAfter: max(s.breaker.RetryAfter(), minRetryAfter), Cause: cause}
}

func (s *Service) report(kind string) {
	if s.hooks.OnFallback != nil {
		s.hooks.OnFallback(s.name, kind)
	}
}

func isJSONObject(body []byte) bool {
	var obj map[string]json.RawMessage
	return json.Unmarshal(body, &obj) == nil
}

// lru is a bounded cache of the last good answer per key.
type lru struct {
	mu    sync.Mutex
	size  int
	order *list.List
	items map[string]*list.Element
}

type entry struct {
	key  string
	body json.RawMessage
	at   time.Time
}

func newLRU(size int) *lru {
	return &lru{size: size, order: list.New(), items: make(map[string]*list.Element, size)}
}

func (c *lru) put(key string, body json.RawMessage, at time.Time) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if el, ok := c.items[key]; ok {
		el.Value = entry{key: key, body: body, at: at}
		c.order.MoveToFront(el)
		return
	}
	c.items[key] = c.order.PushFront(entry{key: key, body: body, at: at})
	if c.order.Len() > c.size {
		oldest := c.order.Back()
		c.order.Remove(oldest)
		delete(c.items, oldest.Value.(entry).key)
	}
}

func (c *lru) get(key string) (json.RawMessage, time.Time, bool) {
	c.mu.Lock()
	defer c.mu.Unlock()
	el, ok := c.items[key]
	if !ok {
		return nil, time.Time{}, false
	}
	c.order.MoveToFront(el)
	e := el.Value.(entry)
	return e.body, e.at, true
}
