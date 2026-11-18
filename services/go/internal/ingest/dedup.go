package ingest

import (
	"context"
	"time"

	"github.com/redis/go-redis/v9"
)

// RedisDedup keeps the dedup keys of delivered messages in Redis (ADR-028 §2).
type RedisDedup struct{ RDB redis.Cmdable }

// Seen reports whether key exists.
func (d RedisDedup) Seen(ctx context.Context, key string) (bool, error) {
	n, err := d.RDB.Exists(ctx, key).Result()
	return n > 0, err
}

// Mark writes key with ttl.
func (d RedisDedup) Mark(ctx context.Context, key string, ttl time.Duration) error {
	return d.RDB.Set(ctx, key, 1, ttl).Err()
}
