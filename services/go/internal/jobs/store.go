// Package jobs keeps asynchronous graph analysis jobs in Redis, so any gateway replica can
// answer a poll (US-302 AC2, AC5, AC6; ADR-010 §5), and runs them on a bounded worker pool.
package jobs

import (
	"context"
	"encoding/json"
	"errors"
	"strconv"
	"strings"
	"time"

	"github.com/redis/go-redis/v9"
)

// Status of a job.
type Status string

// Job states, in order.
const (
	StatusQueued    Status = "queued"
	StatusRunning   Status = "running"
	StatusSucceeded Status = "succeeded"
	StatusFailed    Status = "failed"
)

const (
	keyPrefix = "eop:gateway:job:"
	indexKey  = "eop:gateway:jobs"
)

// ErrNotFound means the job never existed or already expired.
var ErrNotFound = errors.New("jobs: job not found")

// Failure is why a job failed, in public terms.
type Failure struct {
	Code    string `json:"code"`
	Message string `json:"message"`
	Field   string `json:"field,omitempty"`
}

// Job is one asynchronous analysis.
type Job struct {
	ID          string
	Status      Status
	CreatedAt   time.Time
	StartedAt   time.Time
	CompletedAt time.Time
	NodeCount   int
	EdgeCount   int
	Algorithms  []string
	Failure     *Failure
	Result      json.RawMessage
}

// Store persists jobs in Redis. A job hash expires JobMaxAge after creation while it is
// unfinished and ResultTTL after it finishes; the index sorted set is pruned on every listing.
type Store struct {
	rdb       redis.UniversalClient
	resultTTL time.Duration
	maxAge    time.Duration
	now       func() time.Time
}

// NewStore wraps a Redis client.
func NewStore(rdb redis.UniversalClient, resultTTL, maxAge time.Duration) *Store {
	return &Store{rdb: rdb, resultTTL: resultTTL, maxAge: maxAge, now: time.Now}
}

// Ping checks that Redis answers.
func (s *Store) Ping(ctx context.Context) error {
	return s.rdb.Ping(ctx).Err()
}

func key(id string) string { return keyPrefix + id }

// Create stores a queued job.
func (s *Store) Create(ctx context.Context, job Job) error {
	_, err := s.rdb.TxPipelined(ctx, func(p redis.Pipeliner) error {
		p.HSet(ctx, key(job.ID), map[string]any{
			"status":     string(StatusQueued),
			"created_at": formatTime(job.CreatedAt),
			"node_count": job.NodeCount,
			"edge_count": job.EdgeCount,
			"algorithms": strings.Join(job.Algorithms, ","),
		})
		p.Expire(ctx, key(job.ID), s.maxAge)
		p.ZAdd(ctx, indexKey, redis.Z{Score: float64(job.CreatedAt.UnixMilli()), Member: job.ID})
		return nil
	})
	return err
}

// Delete removes a job that was never accepted (for example, the queue was full).
func (s *Store) Delete(ctx context.Context, id string) error {
	_, err := s.rdb.TxPipelined(ctx, func(p redis.Pipeliner) error {
		p.Del(ctx, key(id))
		p.ZRem(ctx, indexKey, id)
		return nil
	})
	return err
}

// updateIfExists applies fields and a new TTL only if the job still exists, so a late update
// cannot resurrect an expired job as a partial hash.
var updateIfExists = redis.NewScript(`
if redis.call('EXISTS', KEYS[1]) == 0 then return 0 end
redis.call('HSET', KEYS[1], unpack(ARGV, 2))
if tonumber(ARGV[1]) > 0 then redis.call('PEXPIRE', KEYS[1], ARGV[1]) end
return 1`)

func (s *Store) update(ctx context.Context, id string, ttl time.Duration, fields ...string) error {
	args := make([]any, 0, len(fields)+1)
	args = append(args, ttl.Milliseconds())
	for _, f := range fields {
		args = append(args, f)
	}
	updated, err := updateIfExists.Run(ctx, s.rdb, []string{key(id)}, args...).Int()
	if err != nil {
		return err
	}
	if updated == 0 {
		return ErrNotFound
	}
	return nil
}

// MarkRunning records that a worker started the job.
func (s *Store) MarkRunning(ctx context.Context, id string) error {
	return s.update(ctx, id, 0, "status", string(StatusRunning), "started_at", formatTime(s.now()))
}

// Complete stores the result; the job stays readable for ResultTTL (US-302 AC5).
func (s *Store) Complete(ctx context.Context, id string, result json.RawMessage) error {
	return s.update(ctx, id, s.resultTTL,
		"status", string(StatusSucceeded), "completed_at", formatTime(s.now()), "result", string(result))
}

// Fail stores why the job failed; the job stays readable for ResultTTL.
func (s *Store) Fail(ctx context.Context, id string, failure Failure) error {
	return s.update(ctx, id, s.resultTTL,
		"status", string(StatusFailed), "completed_at", formatTime(s.now()),
		"error_code", failure.Code, "error_message", failure.Message, "error_field", failure.Field)
}

// Get returns a job with its result.
func (s *Store) Get(ctx context.Context, id string) (Job, error) {
	fields, err := s.rdb.HGetAll(ctx, key(id)).Result()
	if err != nil {
		return Job{}, err
	}
	if len(fields) == 0 {
		return Job{}, ErrNotFound
	}
	return decode(id, fields), nil
}

var summaryFields = []string{"status", "created_at", "started_at", "completed_at", "node_count", "edge_count", "algorithms", "error_code", "error_message", "error_field"}

// List returns up to limit recent jobs, newest first, without results (US-302 AC6).
func (s *Store) List(ctx context.Context, limit int) ([]Job, error) {
	cutoff := s.now().Add(-s.maxAge).UnixMilli()
	if err := s.rdb.ZRemRangeByScore(ctx, indexKey, "-inf", "("+strconv.FormatInt(cutoff, 10)).Err(); err != nil {
		return nil, err
	}
	// Finished jobs expire before the index entry is pruned; read a margin and skip them.
	ids, err := s.rdb.ZRevRange(ctx, indexKey, 0, int64(limit*2-1)).Result()
	if err != nil {
		return nil, err
	}
	cmds := make([]*redis.SliceCmd, len(ids))
	if _, err := s.rdb.Pipelined(ctx, func(p redis.Pipeliner) error {
		for i, id := range ids {
			cmds[i] = p.HMGet(ctx, key(id), summaryFields...)
		}
		return nil
	}); err != nil {
		return nil, err
	}
	jobs := make([]Job, 0, limit)
	for i, cmd := range cmds {
		values := cmd.Val()
		if values[0] == nil { // expired
			continue
		}
		fields := make(map[string]string, len(summaryFields))
		for j, name := range summaryFields {
			if v, ok := values[j].(string); ok {
				fields[name] = v
			}
		}
		jobs = append(jobs, decode(ids[i], fields))
		if len(jobs) == limit {
			break
		}
	}
	return jobs, nil
}

func decode(id string, f map[string]string) Job {
	job := Job{
		ID:          id,
		Status:      Status(f["status"]),
		CreatedAt:   parseTime(f["created_at"]),
		StartedAt:   parseTime(f["started_at"]),
		CompletedAt: parseTime(f["completed_at"]),
	}
	job.NodeCount, _ = strconv.Atoi(f["node_count"])
	job.EdgeCount, _ = strconv.Atoi(f["edge_count"])
	if f["algorithms"] != "" {
		job.Algorithms = strings.Split(f["algorithms"], ",")
	}
	if f["error_code"] != "" {
		job.Failure = &Failure{Code: f["error_code"], Message: f["error_message"], Field: f["error_field"]}
	}
	if f["result"] != "" {
		job.Result = json.RawMessage(f["result"])
	}
	return job
}

func formatTime(t time.Time) string { return t.UTC().Format(time.RFC3339Nano) }

func parseTime(s string) time.Time {
	t, _ := time.Parse(time.RFC3339Nano, s) // zero time when absent
	return t
}
