package jobs

import (
	"context"
	"encoding/json"
	"errors"
	"log/slog"
	"sync"
	"time"

	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/attribute"
	"go.opentelemetry.io/otel/trace"

	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/analysis"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/eopclient"
)

// storeWriteTimeout bounds each Redis write a worker makes, including during shutdown.
const storeWriteTimeout = 5 * time.Second

// ErrQueueFull means every worker is busy and the queue is at capacity.
var ErrQueueFull = errors.New("jobs: queue is full")

// ErrStopped means the runner no longer accepts jobs because the gateway is shutting down.
var ErrStopped = errors.New("jobs: runner is stopping")

// Analyzer runs one analysis; *eopclient.Client implements it.
type Analyzer interface {
	AnalyzeGraph(ctx context.Context, requestID string, req eopclient.AnalyzeRequest) (json.RawMessage, error)
}

type task struct {
	id     string
	req    eopclient.AnalyzeRequest
	parent trace.SpanContext
}

// Runner executes jobs in this pod. Jobs live in Redis, but execution does not move between
// pods: a job whose pod dies stays unfinished until JobMaxAge expires it.
type Runner struct {
	store    *Store
	analyzer Analyzer
	logger   *slog.Logger
	queue    chan task

	mu       sync.Mutex
	stopping bool

	runCtx    context.Context
	cancelRun context.CancelFunc
	wg        sync.WaitGroup
}

// NewRunner starts workers goroutines reading a queue of the given capacity.
func NewRunner(store *Store, analyzer Analyzer, logger *slog.Logger, workers, capacity int) *Runner {
	runCtx, cancel := context.WithCancel(context.Background())
	r := &Runner{
		store: store, analyzer: analyzer, logger: logger,
		queue: make(chan task, capacity), runCtx: runCtx, cancelRun: cancel,
	}
	for i := 0; i < workers; i++ {
		r.wg.Add(1)
		go r.work()
	}
	return r
}

// Submit stores a queued job and hands it to a worker. The request context only carries the
// trace: the job outlives the HTTP request that created it.
func (r *Runner) Submit(ctx context.Context, job Job, req eopclient.AnalyzeRequest) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.stopping {
		return ErrStopped
	}
	if len(r.queue) == cap(r.queue) {
		return ErrQueueFull
	}
	if err := r.store.Create(ctx, job); err != nil {
		return err
	}
	// Cannot block: the queue had room and only Submit sends, under the same lock.
	r.queue <- task{id: job.ID, req: req, parent: trace.SpanContextFromContext(ctx)}
	return nil
}

func (r *Runner) work() {
	defer r.wg.Done()
	for t := range r.queue {
		r.run(t)
	}
}

func (r *Runner) run(t task) {
	ctx := trace.ContextWithSpanContext(r.runCtx, t.parent)
	ctx, span := otel.Tracer("api-gateway/jobs").Start(ctx, "graph.analyze.job",
		trace.WithAttributes(attribute.String("job.id", t.id), attribute.Int("graph.nodes", len(t.req.Nodes))))
	defer span.End()

	if r.runCtx.Err() != nil {
		r.fail(ctx, t.id, context.Canceled)
		return
	}
	if err := r.write(ctx, func(c context.Context) error { return r.store.MarkRunning(c, t.id) }); err != nil {
		r.logger.ErrorContext(ctx, "job could not be marked running", "job_id", t.id, "error", err)
		return
	}
	result, err := r.analyzer.AnalyzeGraph(ctx, t.id, t.req)
	if err != nil {
		span.RecordError(err)
		r.fail(ctx, t.id, err)
		return
	}
	if err := r.write(ctx, func(c context.Context) error { return r.store.Complete(c, t.id, result) }); err != nil {
		r.logger.ErrorContext(ctx, "job result could not be stored", "job_id", t.id, "error", err)
		return
	}
	r.logger.InfoContext(ctx, "job succeeded", "job_id", t.id)
}

func (r *Runner) fail(ctx context.Context, id string, cause error) {
	outcome := analysis.Classify(cause)
	failure := Failure{Code: outcome.Code, Message: outcome.Message, Field: outcome.Field}
	r.logger.WarnContext(ctx, "job failed", "job_id", id, "code", outcome.Code, "error", cause)
	if err := r.write(ctx, func(c context.Context) error { return r.store.Fail(c, id, failure) }); err != nil {
		r.logger.ErrorContext(ctx, "job failure could not be stored", "job_id", id, "error", err)
	}
}

// write runs a store update with its own deadline, detached from the run context so results
// are still recorded while the runner shuts down.
func (r *Runner) write(ctx context.Context, update func(context.Context) error) error {
	c, cancel := context.WithTimeout(context.WithoutCancel(ctx), storeWriteTimeout)
	defer cancel()
	return update(c)
}

// Stop refuses new jobs and waits for workers to finish what is queued or running. When ctx
// ends first, running analyses are cancelled and every unfinished job is recorded as failed
// with GATEWAY_SHUTDOWN, so pollers get an answer instead of a job stuck in "running".
func (r *Runner) Stop(ctx context.Context) error {
	r.mu.Lock()
	if !r.stopping {
		r.stopping = true
		close(r.queue)
	}
	r.mu.Unlock()

	done := make(chan struct{})
	go func() {
		r.wg.Wait()
		close(done)
	}()
	select {
	case <-done:
		r.cancelRun()
		return nil
	case <-ctx.Done():
		r.cancelRun()
		<-done
		return ctx.Err()
	}
}
