package edge

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"sort"
	"sync"
	"time"

	"github.com/redis/go-redis/v9"
)

// StatusKey holds the ingestor's snapshot (ADR-028 §4); it expires when the ingestor stops writing.
const StatusKey = "edge:status"

// Ingestor connection states, as /v1/edge/status reports them.
const (
	StateConnected    = "connected"
	StateReconnecting = "reconnecting"
	StateDisconnected = "disconnected"
)

// Device is one device's connectivity in the snapshot.
type Device struct {
	NodeID    string    `json:"node_id"`
	LastSeen  time.Time `json:"last_seen"`
	Connected bool      `json:"connected"`
}

// Snapshot is the body of GET /v1/edge/status (protocol §4) and the value of StatusKey.
type Snapshot struct {
	State         string   `json:"state"`
	WindowSeconds int      `json:"window_seconds"`
	Received      uint64   `json:"received"`
	Forwarded     uint64   `json:"forwarded"`
	Duplicates    uint64   `json:"duplicates"`
	Invalid       uint64   `json:"invalid"`
	Devices       []Device `json:"devices"`
}

// Disconnected is what the gateway answers when no ingestor wrote a snapshot recently.
func Disconnected(window time.Duration) Snapshot {
	return Snapshot{State: StateDisconnected, WindowSeconds: int(window / time.Second), Devices: []Device{}}
}

// WriteSnapshot stores s under StatusKey with ttl.
func WriteSnapshot(ctx context.Context, rdb redis.Cmdable, s Snapshot, ttl time.Duration) error {
	raw, err := json.Marshal(s)
	if err != nil {
		return err
	}
	return rdb.Set(ctx, StatusKey, raw, ttl).Err()
}

// ReadSnapshot returns the stored snapshot; ok is false when there is none (ingestor down).
func ReadSnapshot(ctx context.Context, rdb redis.Cmdable) (s Snapshot, ok bool, err error) {
	raw, err := rdb.Get(ctx, StatusKey).Bytes()
	if errors.Is(err, redis.Nil) {
		return Snapshot{}, false, nil
	}
	if err != nil {
		return Snapshot{}, false, err
	}
	if err := json.Unmarshal(raw, &s); err != nil {
		return Snapshot{}, false, fmt.Errorf("edge: unreadable snapshot: %w", err)
	}
	if s.Devices == nil {
		s.Devices = []Device{}
	}
	return s, true, nil
}

// Counter names of a Window.
const (
	CountReceived   = "received"
	CountForwarded  = "forwarded"
	CountDuplicates = "duplicates"
	CountInvalid    = "invalid"
)

// Window counts events per second over the last `size` (60 s for the snapshot) and remembers
// when each device was last seen. It is safe for concurrent use.
type Window struct {
	mu       sync.Mutex
	size     time.Duration
	buckets  map[int64]map[string]uint64 // unix second → counter → count
	lastSeen map[string]time.Time
}

// NewWindow returns a window of `size`.
func NewWindow(size time.Duration) *Window {
	return &Window{size: size, buckets: map[int64]map[string]uint64{}, lastSeen: map[string]time.Time{}}
}

// Add counts one event of `counter` at `at`.
func (w *Window) Add(counter string, at time.Time) {
	w.mu.Lock()
	defer w.mu.Unlock()
	sec := at.Unix()
	if w.buckets[sec] == nil {
		w.buckets[sec] = map[string]uint64{}
	}
	w.buckets[sec][counter]++
}

// Seen records that a device published at `at`.
func (w *Window) Seen(nodeID string, at time.Time) {
	w.mu.Lock()
	defer w.mu.Unlock()
	if at.After(w.lastSeen[nodeID]) {
		w.lastSeen[nodeID] = at
	}
}

// Snapshot summarises the window at `now`; a device is connected if seen within `liveness`.
func (w *Window) Snapshot(now time.Time, state string, liveness time.Duration) Snapshot {
	w.mu.Lock()
	defer w.mu.Unlock()
	oldest := now.Add(-w.size).Unix()
	s := Snapshot{State: state, WindowSeconds: int(w.size / time.Second), Devices: []Device{}}
	for sec, counts := range w.buckets {
		if sec <= oldest {
			delete(w.buckets, sec)
			continue
		}
		s.Received += counts[CountReceived]
		s.Forwarded += counts[CountForwarded]
		s.Duplicates += counts[CountDuplicates]
		s.Invalid += counts[CountInvalid]
	}
	for nodeID, seen := range w.lastSeen {
		s.Devices = append(s.Devices, Device{NodeID: nodeID, LastSeen: seen.UTC(), Connected: now.Sub(seen) <= liveness})
	}
	sort.Slice(s.Devices, func(i, j int) bool { return s.Devices[i].NodeID < s.Devices[j].NodeID })
	return s
}
