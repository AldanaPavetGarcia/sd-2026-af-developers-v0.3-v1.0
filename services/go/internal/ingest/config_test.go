package ingest

import (
	"bytes"
	"context"
	"errors"
	"log/slog"
	"strings"
	"testing"
	"time"

	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/edge"
)

func env(values map[string]string) func(string) string {
	return func(key string) string { return values[key] }
}

func TestLoadDefaults(t *testing.T) {
	cfg, err := Load(env(nil))
	if err != nil {
		t.Fatal(err)
	}
	if cfg.MQTT.Broker != DefaultBroker || cfg.MQTT.ClientID != DefaultClientID || cfg.TopicPrefix != DefaultTopicPrefix ||
		cfg.DedupTTL != 5*time.Minute || cfg.Retry.Initial != 100*time.Millisecond || cfg.Retry.Max != 5*time.Second ||
		cfg.StatusInterval != time.Second || cfg.StatusTTL != 5*time.Second || cfg.Window != time.Minute ||
		!cfg.TracingEnabled || cfg.Server.Address != DefaultServerAddr {
		t.Fatalf("%+v", cfg)
	}
}

func TestLoadOverrides(t *testing.T) {
	cfg, err := Load(env(map[string]string{
		"EOP_MQTT_BROKER": "tcp://mosquitto:1883", "EOP_MQTT_USERNAME": "ingestor", "EOP_MQTT_PASSWORD": "s3cret",
		"EOP_SERVER_ADDR": "eop-server:9026", "EOP_REDIS_DB": "2", "EOP_EDGE_DEDUP_TTL": "10m", "EOP_TRACING_ENABLED": "0",
		"EOP_EDGE_QUEUE": "10",
	}))
	if err != nil {
		t.Fatal(err)
	}
	if cfg.MQTT.Broker != "tcp://mosquitto:1883" || cfg.MQTT.Username != "ingestor" || cfg.RedisDB != 2 ||
		cfg.DedupTTL != 10*time.Minute || cfg.TracingEnabled || cfg.MQTT.Queue != 10 {
		t.Fatalf("%+v", cfg)
	}
}

func TestLoadRejects(t *testing.T) {
	cases := map[string]map[string]string{
		"bad duration":   {"EOP_EDGE_DEDUP_TTL": "forever"},
		"bad integer":    {"EOP_REDIS_DB": "two"},
		"bad boolean":    {"EOP_TRACING_ENABLED": "maybe"},
		"negative db":    {"EOP_REDIS_DB": "-1"},
		"zero duration":  {"EOP_EDGE_WINDOW": "0s"},
		"retry max":      {"EOP_EDGE_RETRY_MAX": "10ms"},
		"status ttl":     {"EOP_EDGE_STATUS_TTL": "1s"},
		"queue":          {"EOP_EDGE_QUEUE": "0"},
		"server timeout": {"EOP_SERVER_REQUEST_TIMEOUT": "0s"},
	}
	for name, values := range cases {
		t.Run(name, func(t *testing.T) {
			if _, err := Load(env(values)); err == nil {
				t.Fatal("accepted")
			}
		})
	}
}

func TestPublishStatusLogsFailuresOnce(t *testing.T) {
	var logs bytes.Buffer
	logger := slog.New(slog.NewTextHandler(&logs, nil))
	window := edge.NewWindow(time.Minute)
	writes := make(chan edge.Snapshot, 64)
	failures := 3
	write := func(_ context.Context, s edge.Snapshot, ttl time.Duration) error {
		writes <- s
		if failures > 0 {
			failures--
			return errors.New("redis down")
		}
		return nil
	}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() {
		PublishStatus(ctx, time.Millisecond, time.Second, 10*time.Second, window,
			func() string { return edge.StateConnected }, write, time.Now, logger)
		close(done)
	}()
	for i := 0; i < 5; i++ {
		if s := <-writes; s.State != edge.StateConnected {
			t.Fatalf("%+v", s)
		}
	}
	cancel()
	<-done
	if strings.Count(logs.String(), "could not publish") != 1 || !strings.Contains(logs.String(), "published again") {
		t.Fatalf("logs:\n%s", logs.String())
	}
}

func TestSubscriberStateBeforeConnecting(t *testing.T) {
	s := &Subscriber{}
	if s.State() != edge.StateReconnecting {
		t.Fatal(s.State())
	}
}

func TestRunFailsOnABadMetricsAddress(t *testing.T) {
	cfg, err := Load(env(map[string]string{"EOP_EDGE_METRICS_ADDR": "256.0.0.1:1", "EOP_TRACING_ENABLED": "false"}))
	if err != nil {
		t.Fatal(err)
	}
	if err := Run(context.Background(), cfg, slog.New(slog.NewTextHandler(&bytes.Buffer{}, nil)), nil); err == nil {
		t.Fatal("a bad metrics address must stop the ingestor")
	}
}
