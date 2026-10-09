package jobs

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"sync"
	"testing"
	"time"

	"github.com/alicebob/miniredis/v2"
	"github.com/redis/go-redis/v9"

	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/eopclient"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/telemetry"
)

const (
	resultTTL = 5 * time.Minute
	maxAge    = 30 * time.Minute
)

// testRedis moves the store's clock and miniredis' TTLs together.
type testRedis struct {
	*miniredis.Miniredis
	mu  sync.Mutex
	now time.Time
}

func (r *testRedis) Now() time.Time {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.now
}

func (r *testRedis) FastForward(d time.Duration) {
	r.mu.Lock()
	r.now = r.now.Add(d)
	r.mu.Unlock()
	r.Miniredis.FastForward(d)
}

func newStore(t *testing.T) (*Store, *testRedis) {
	t.Helper()
	mr := &testRedis{Miniredis: miniredis.RunT(t), now: time.Date(2026, 10, 8, 12, 0, 0, 0, time.UTC)}
	rdb := redis.NewClient(&redis.Options{Addr: mr.Addr(), MaxRetries: -1, DialTimeout: 200 * time.Millisecond})
	t.Cleanup(func() { _ = rdb.Close() })
	store := NewStore(rdb, resultTTL, maxAge)
	store.now = mr.Now
	return store, mr
}

func sampleJob(id string, created time.Time) Job {
	return Job{ID: id, CreatedAt: created, NodeCount: 1200, EdgeCount: 3000, Algorithms: []string{"connected_components", "centrality"}}
}

func TestStoreLifecycle(t *testing.T) {
	store, mr := newStore(t)
	ctx := context.Background()
	created := mr.Now()
	if err := store.Create(ctx, sampleJob("j1", created)); err != nil {
		t.Fatal(err)
	}
	job, err := store.Get(ctx, "j1")
	if err != nil || job.Status != StatusQueued || job.NodeCount != 1200 || len(job.Algorithms) != 2 || !job.CreatedAt.Equal(created) {
		t.Fatalf("queued job = %+v, %v", job, err)
	}
	if ttl := mr.TTL(key("j1")); ttl != maxAge {
		t.Fatalf("unfinished job TTL = %v, want %v", ttl, maxAge)
	}

	mr.FastForward(2 * time.Second)
	if err := store.MarkRunning(ctx, "j1"); err != nil {
		t.Fatal(err)
	}
	mr.FastForward(3 * time.Second)
	if err := store.Complete(ctx, "j1", json.RawMessage(`{"graph_size":{"nodes":1200}}`)); err != nil {
		t.Fatal(err)
	}
	job, _ = store.Get(ctx, "j1")
	if job.Status != StatusSucceeded || string(job.Result) != `{"graph_size":{"nodes":1200}}` ||
		job.StartedAt.Sub(created) != 2*time.Second || job.CompletedAt.Sub(created) != 5*time.Second {
		t.Fatalf("completed job = %+v", job)
	}

	// US-302 AC5: still readable just before 5 minutes after completion, gone after.
	mr.FastForward(resultTTL - time.Second)
	if _, err := store.Get(ctx, "j1"); err != nil {
		t.Fatalf("result evicted before 5 minutes: %v", err)
	}
	mr.FastForward(2 * time.Second)
	if _, err := store.Get(ctx, "j1"); !errors.Is(err, ErrNotFound) {
		t.Fatalf("Get after TTL = %v, want ErrNotFound", err)
	}
}

func TestStoreFail(t *testing.T) {
	store, mr := newStore(t)
	ctx := context.Background()
	_ = store.Create(ctx, sampleJob("j1", mr.Now()))
	if err := store.Fail(ctx, "j1", Failure{Code: "INVALID_GRAPH", Message: "bad edge", Field: "edges[0].to"}); err != nil {
		t.Fatal(err)
	}
	job, _ := store.Get(ctx, "j1")
	if job.Status != StatusFailed || job.Failure == nil || job.Failure.Field != "edges[0].to" || job.Result != nil {
		t.Fatalf("failed job = %+v", job)
	}
	if ttl := mr.TTL(key("j1")); ttl != resultTTL {
		t.Fatalf("failed job TTL = %v, want %v", ttl, resultTTL)
	}
}

func TestUpdatesDoNotResurrectExpiredJobs(t *testing.T) {
	store, _ := newStore(t)
	ctx := context.Background()
	for name, update := range map[string]func() error{
		"running":  func() error { return store.MarkRunning(ctx, "ghost") },
		"complete": func() error { return store.Complete(ctx, "ghost", json.RawMessage(`{}`)) },
		"fail":     func() error { return store.Fail(ctx, "ghost", Failure{Code: "X"}) },
	} {
		if err := update(); !errors.Is(err, ErrNotFound) {
			t.Errorf("%s on a missing job = %v, want ErrNotFound", name, err)
		}
	}
	if _, err := store.Get(ctx, "ghost"); !errors.Is(err, ErrNotFound) {
		t.Fatal("an update recreated a missing job")
	}
}

func TestListNewestFirstWithoutResultsAndSkipsExpired(t *testing.T) {
	store, mr := newStore(t)
	ctx := context.Background()
	for _, id := range []string{"old", "mid", "new"} {
		_ = store.Create(ctx, sampleJob(id, mr.Now()))
		mr.FastForward(time.Second)
	}
	_ = store.Complete(ctx, "mid", json.RawMessage(`{"big":"result"}`))
	_ = store.Delete(ctx, "old")

	jobs, err := store.List(ctx, 10)
	if err != nil {
		t.Fatal(err)
	}
	if len(jobs) != 2 || jobs[0].ID != "new" || jobs[1].ID != "mid" || jobs[1].Status != StatusSucceeded {
		t.Fatalf("List = %+v", jobs)
	}
	if jobs[1].Result != nil {
		t.Fatal("List must not carry results")
	}
	if limited, _ := store.List(ctx, 1); len(limited) != 1 || limited[0].ID != "new" {
		t.Fatalf("List(1) = %+v", limited)
	}

	mr.FastForward(resultTTL + time.Second) // "mid" expires; "new" is still within maxAge
	if jobs, _ := store.List(ctx, 10); len(jobs) != 1 || jobs[0].ID != "new" {
		t.Fatalf("List after expiry = %+v", jobs)
	}
	mr.FastForward(maxAge)
	if n, _ := mr.ZMembers(indexKey); len(n) != 1 {
		// the hash of "new" expired; the index entry is pruned on the next List
		t.Logf("index before pruning: %v", n)
	}
	if jobs, _ := store.List(ctx, 10); len(jobs) != 0 {
		t.Fatalf("List after maxAge = %+v", jobs)
	}
	if members, _ := mr.ZMembers(indexKey); len(members) != 0 {
		t.Fatalf("index not pruned: %v", members)
	}
}

func TestStorePropagatesRedisErrors(t *testing.T) {
	store, mr := newStore(t)
	ctx := context.Background()
	mr.Close()
	if store.Ping(ctx) == nil || store.Create(ctx, sampleJob("j", time.Now())) == nil {
		t.Fatal("Redis down must surface as an error")
	}
	if _, err := store.Get(ctx, "j"); err == nil || errors.Is(err, ErrNotFound) {
		t.Fatalf("Get with Redis down = %v, want a connection error", err)
	}
	if _, err := store.List(ctx, 5); err == nil {
		t.Fatal("List with Redis down must fail")
	}
	if err := store.MarkRunning(ctx, "j"); err == nil {
		t.Fatal("MarkRunning with Redis down must fail")
	}
}

// fakeAnalyzer answers from a function and can block until released.
type fakeAnalyzer struct {
	mu      sync.Mutex
	calls   []string
	answer  func(ctx context.Context, id string) (json.RawMessage, error)
	started chan string
}

func (f *fakeAnalyzer) AnalyzeGraph(ctx context.Context, id string, _ eopclient.AnalyzeRequest) (json.RawMessage, error) {
	f.mu.Lock()
	f.calls = append(f.calls, id)
	f.mu.Unlock()
	if f.started != nil {
		f.started <- id
	}
	return f.answer(ctx, id)
}

func waitFor(t *testing.T, store *Store, id string, want Status) Job {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if job, err := store.Get(context.Background(), id); err == nil && job.Status == want {
			return job
		}
		time.Sleep(10 * time.Millisecond)
	}
	job, err := store.Get(context.Background(), id)
	t.Fatalf("job %s = %+v, %v; want %s", id, job, err, want)
	return Job{}
}

var logger = telemetry.NewLogger(io.Discard, "api-gateway")

func TestRunnerCompletesAndFailsJobs(t *testing.T) {
	store, mr := newStore(t)
	analyzer := &fakeAnalyzer{answer: func(_ context.Context, id string) (json.RawMessage, error) {
		if id == "bad" {
			return nil, &eopclient.AnalysisError{Code: "ERROR_INVALID_GRAPH", Message: "edge to nowhere", Field: "payload.edges[0].to"}
		}
		if id == "offline" {
			return nil, &eopclient.ProtocolError{Code: eopclient.CodeServiceUnavailable}
		}
		return json.RawMessage(`{"ok":true}`), nil
	}}
	runner := NewRunner(store, analyzer, logger, 1, 4)
	defer func() { _ = runner.Stop(context.Background()) }()

	for _, id := range []string{"good", "bad", "offline"} {
		if err := runner.Submit(context.Background(), sampleJob(id, mr.Now()), eopclient.AnalyzeRequest{}); err != nil {
			t.Fatal(err)
		}
	}
	if job := waitFor(t, store, "good", StatusSucceeded); string(job.Result) != `{"ok":true}` {
		t.Fatalf("result = %s", job.Result)
	}
	if job := waitFor(t, store, "bad", StatusFailed); job.Failure.Code != "INVALID_GRAPH" || job.Failure.Field != "payload.edges[0].to" {
		t.Fatalf("failure = %+v", job.Failure)
	}
	if job := waitFor(t, store, "offline", StatusFailed); job.Failure.Code != "HPC_UNAVAILABLE" {
		t.Fatalf("failure = %+v", job.Failure)
	}
}

func TestRunnerRefusesWhenQueueIsFull(t *testing.T) {
	store, mr := newStore(t)
	release := make(chan struct{})
	analyzer := &fakeAnalyzer{started: make(chan string, 4), answer: func(context.Context, string) (json.RawMessage, error) {
		<-release
		return json.RawMessage(`{}`), nil
	}}
	runner := NewRunner(store, analyzer, logger, 1, 1)
	defer func() { close(release); _ = runner.Stop(context.Background()) }()

	_ = runner.Submit(context.Background(), sampleJob("running", mr.Now()), eopclient.AnalyzeRequest{})
	<-analyzer.started // the only worker is busy
	if err := runner.Submit(context.Background(), sampleJob("queued", mr.Now()), eopclient.AnalyzeRequest{}); err != nil {
		t.Fatalf("first queued job refused: %v", err)
	}
	if err := runner.Submit(context.Background(), sampleJob("overflow", mr.Now()), eopclient.AnalyzeRequest{}); !errors.Is(err, ErrQueueFull) {
		t.Fatalf("error = %v, want ErrQueueFull", err)
	}
	if _, err := store.Get(context.Background(), "overflow"); !errors.Is(err, ErrNotFound) {
		t.Fatal("a refused job must not be stored")
	}
}

func TestRunnerReportsStoreErrorsOnSubmit(t *testing.T) {
	store, mr := newStore(t)
	runner := NewRunner(store, &fakeAnalyzer{answer: func(context.Context, string) (json.RawMessage, error) { return nil, nil }}, logger, 1, 1)
	defer func() { _ = runner.Stop(context.Background()) }()
	mr.Close()
	if err := runner.Submit(context.Background(), sampleJob("j", time.Now()), eopclient.AnalyzeRequest{}); err == nil {
		t.Fatal("Submit with Redis down must fail")
	}
}

func TestStopFinishesQueuedWorkWhenThereIsTime(t *testing.T) {
	store, mr := newStore(t)
	analyzer := &fakeAnalyzer{answer: func(context.Context, string) (json.RawMessage, error) { return json.RawMessage(`{}`), nil }}
	runner := NewRunner(store, analyzer, logger, 1, 4)
	for _, id := range []string{"a", "b"} {
		_ = runner.Submit(context.Background(), sampleJob(id, mr.Now()), eopclient.AnalyzeRequest{})
	}
	if err := runner.Stop(context.Background()); err != nil {
		t.Fatal(err)
	}
	waitFor(t, store, "a", StatusSucceeded)
	waitFor(t, store, "b", StatusSucceeded)
	if err := runner.Submit(context.Background(), sampleJob("late", mr.Now()), eopclient.AnalyzeRequest{}); !errors.Is(err, ErrStopped) {
		t.Fatalf("Submit after Stop = %v, want ErrStopped", err)
	}
	if err := runner.Stop(context.Background()); err != nil {
		t.Fatal("Stop must be idempotent")
	}
}

func TestStopCancelsRunningAndQueuedJobsWhenOutOfTime(t *testing.T) {
	store, mr := newStore(t)
	analyzer := &fakeAnalyzer{started: make(chan string, 4), answer: func(ctx context.Context, _ string) (json.RawMessage, error) {
		<-ctx.Done() // an analysis that only ends when cancelled
		return nil, ctx.Err()
	}}
	runner := NewRunner(store, analyzer, logger, 1, 4)
	_ = runner.Submit(context.Background(), sampleJob("running", mr.Now()), eopclient.AnalyzeRequest{})
	<-analyzer.started
	_ = runner.Submit(context.Background(), sampleJob("queued", mr.Now()), eopclient.AnalyzeRequest{})

	ctx, cancel := context.WithTimeout(context.Background(), 50*time.Millisecond)
	defer cancel()
	if err := runner.Stop(ctx); !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("Stop = %v, want deadline exceeded", err)
	}
	for _, id := range []string{"running", "queued"} {
		if job := waitFor(t, store, id, StatusFailed); job.Failure.Code != "GATEWAY_SHUTDOWN" {
			t.Fatalf("%s failure = %+v", id, job.Failure)
		}
	}
	if len(analyzer.calls) != 1 {
		t.Fatalf("the queued job must not start after cancellation; calls = %v", analyzer.calls)
	}
}
