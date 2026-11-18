// Package ingest is the edge ingestor (US-404, ADR-028): it takes device messages from the MQTT
// broker, forwards telemetry to eop-server as TELEMETRY commands, and only then lets the broker
// forget them. Delivery is at least once from the broker and has effect once at the server:
// duplicates are recognised by (node_id, boot_id, message_id) in Redis.
package ingest

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"time"

	"go.opentelemetry.io/otel/attribute"
	"go.opentelemetry.io/otel/codes"
	"go.opentelemetry.io/otel/trace"

	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/edge"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/eopclient"
)

// Forwarder delivers a sample to eop-server. *eopclient.Client implements it.
type Forwarder interface {
	Telemetry(ctx context.Context, s eopclient.Sample) error
}

// Deduper remembers delivered messages (ADR-028 §2).
type Deduper interface {
	// Seen reports whether key was marked.
	Seen(ctx context.Context, key string) (bool, error)
	// Mark records key for ttl.
	Mark(ctx context.Context, key string, ttl time.Duration) error
}

// Message is one MQTT message as the broker delivered it.
type Message struct {
	Topic      string
	Payload    []byte
	ReceivedAt time.Time
}

// Outcome says what happened to a message. Every outcome but Stopped lets the broker forget it.
type Outcome string

const (
	Forwarded Outcome = "forwarded" // the server acknowledged it
	Duplicate Outcome = "duplicate" // already delivered: acknowledged, not forwarded (US-404 AC3)
	Invalid   Outcome = "invalid"   // breaks the contract or the server rejected it for good
	Recorded  Outcome = "recorded"  // an event or health report, kept in metrics
	Stopped   Outcome = "stopped"   // shutting down before delivery: the broker delivers it again
)

// Backoff bounds the retries while eop-server cannot take a sample (US-404 AC5).
type Backoff struct {
	Initial time.Duration // 100 ms
	Max     time.Duration // 5 s
}

// Processor handles messages one at a time, in order (ADR-028 §2).
type Processor struct {
	Prefix   string
	Forward  Forwarder
	Dedup    Deduper
	DedupTTL time.Duration
	Retry    Backoff
	Window   *edge.Window
	Metrics  *Metrics
	Tracer   trace.Tracer
	Logger   *slog.Logger
	Now      func() time.Time
	// Sleep waits d or until ctx ends; nil means a timer.
	Sleep func(ctx context.Context, d time.Duration) error
}

// DedupKey is the Redis key of one delivered message.
func DedupKey(nodeID, bootID string, messageID uint32) string {
	return fmt.Sprintf("edge:dedup:%s:%s:%d", nodeID, bootID, messageID)
}

// Handle processes one message and says whether the broker may forget it.
func (p *Processor) Handle(ctx context.Context, m Message) Outcome {
	nodeID, kind, err := edge.SplitTopic(p.Prefix, m.Topic)
	if err != nil {
		return p.invalid(ctx, m, "", err)
	}
	switch kind {
	case edge.KindTelemetry:
		return p.telemetry(ctx, m, nodeID)
	case edge.KindEvents:
		event, err := edge.ParseEvent(m.Payload, nodeID)
		if err != nil {
			return p.invalid(ctx, m, nodeID, err)
		}
		p.Window.Seen(nodeID, m.ReceivedAt)
		p.Metrics.event(event)
		return Recorded
	case edge.KindHealth:
		health, err := edge.ParseHealth(m.Payload, nodeID)
		if err != nil {
			return p.invalid(ctx, m, nodeID, err)
		}
		p.Window.Seen(nodeID, m.ReceivedAt)
		p.Metrics.health(health)
		return Recorded
	}
	return p.invalid(ctx, m, nodeID, fmt.Errorf("%w: topic kind %q", edge.ErrInvalid, kind))
}

func (p *Processor) invalid(ctx context.Context, m Message, nodeID string, err error) Outcome {
	p.Metrics.Invalid.Inc()
	p.Window.Add(edge.CountInvalid, m.ReceivedAt)
	p.Logger.WarnContext(ctx, "edge message rejected", "topic", m.Topic, "node_id", nodeID, "error", err)
	return Invalid
}

func (p *Processor) telemetry(ctx context.Context, m Message, nodeID string) Outcome {
	p.Metrics.Received.Inc()
	p.Window.Add(edge.CountReceived, m.ReceivedAt)
	t, sampledAt, err := edge.ParseTelemetry(m.Payload, nodeID)
	if err != nil {
		return p.invalid(ctx, m, nodeID, err)
	}
	p.Window.Seen(nodeID, m.ReceivedAt)

	key := DedupKey(t.NodeID, t.BootID, t.MessageID)
	seen, err := p.Dedup.Seen(ctx, key)
	if err != nil {
		// Forwarding twice is harmless (the server keeps the newest sample); not forwarding
		// would lose data. So a dedup store that is down only weakens deduplication.
		p.Logger.WarnContext(ctx, "dedup store unavailable; forwarding without the check", "node_id", nodeID, "error", err)
	}
	if seen {
		p.Metrics.Duplicates.Inc()
		p.Window.Add(edge.CountDuplicates, m.ReceivedAt)
		p.Logger.InfoContext(ctx, "duplicate edge message acknowledged, not forwarded",
			"node_id", t.NodeID, "boot_id", t.BootID, "message_id", t.MessageID)
		return Duplicate
	}

	// ADR-028 §3: the trace starts when the device sampled; edge.ingest covers broker receipt to
	// the server's ACK, and the server's span hangs from it through traceparent=.
	attrs := trace.WithAttributes(attribute.String("edge.node_id", t.NodeID),
		attribute.String("edge.boot_id", t.BootID), attribute.Int64("edge.message_id", int64(t.MessageID)))
	msgCtx, msgSpan := p.Tracer.Start(ctx, "edge.message", trace.WithTimestamp(sampledAt), attrs)
	ingestCtx, ingestSpan := p.Tracer.Start(msgCtx, "edge.ingest", trace.WithTimestamp(m.ReceivedAt), attrs)
	end := func(err error) {
		if err != nil {
			ingestSpan.SetStatus(codes.Error, err.Error())
			msgSpan.SetStatus(codes.Error, err.Error())
		}
		ingestSpan.End()
		msgSpan.End()
	}

	sample := eopclient.Sample{NodeID: t.NodeID, BunkerName: t.BunkerName, MessageID: t.MessageID, BootID: t.BootID,
		Temperature: t.Temperature, Status: t.Status, SampledAt: sampledAt}
	if err := p.deliver(ingestCtx, sample); err != nil {
		end(err)
		if errors.Is(err, errStopped) {
			return Stopped
		}
		return p.invalid(ctx, m, nodeID, err)
	}
	delivered := p.Now()
	end(nil)

	// Only now: marking before the ACK would turn a crash in between into a lost message.
	if err := p.Dedup.Mark(ctx, key, p.DedupTTL); err != nil {
		p.Logger.WarnContext(ctx, "could not record the delivered message for dedup", "key", key, "error", err)
	}
	p.Metrics.Forwarded.Inc()
	p.Window.Add(edge.CountForwarded, m.ReceivedAt)
	p.Metrics.latency(t.NodeID, delivered.Sub(sampledAt))
	return Forwarded
}

var errStopped = errors.New("ingest: stopped before delivery")

// deliver retries until the server takes the sample, rejects it for good, or ctx ends.
func (p *Processor) deliver(ctx context.Context, s eopclient.Sample) error {
	delay := p.Retry.Initial
	for attempt := 1; ; attempt++ {
		err := p.Forward.Telemetry(ctx, s)
		if err == nil {
			return nil
		}
		if eopclient.IsPermanent(err) {
			return fmt.Errorf("eop-server rejected the sample: %w", err)
		}
		if ctx.Err() != nil {
			return errStopped
		}
		p.Metrics.Retries.Inc()
		p.Logger.WarnContext(ctx, "eop-server did not take the sample; retrying",
			"node_id", s.NodeID, "message_id", s.MessageID, "attempt", attempt,
			"next_attempt_ms", delay.Milliseconds(), "error", err)
		if err := p.sleep(ctx, delay); err != nil {
			return errStopped
		}
		delay = min(delay*2, p.Retry.Max)
	}
}

func (p *Processor) sleep(ctx context.Context, d time.Duration) error {
	if p.Sleep != nil {
		return p.Sleep(ctx, d)
	}
	timer := time.NewTimer(d)
	defer timer.Stop()
	select {
	case <-ctx.Done():
		return ctx.Err()
	case <-timer.C:
		return nil
	}
}
