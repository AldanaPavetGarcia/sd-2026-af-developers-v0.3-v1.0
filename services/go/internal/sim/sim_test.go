package sim

import (
	"context"
	"errors"
	"io"
	"log/slog"
	"net"
	"strings"
	"sync"
	"testing"
	"time"

	mochi "github.com/mochi-mqtt/server/v2"
	"github.com/mochi-mqtt/server/v2/hooks/auth"
	"github.com/mochi-mqtt/server/v2/listeners"
	"github.com/mochi-mqtt/server/v2/packets"

	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/edge"
)

var quiet = slog.New(slog.NewTextHandler(io.Discard, nil))

func TestTenthsKeepOneDecimal(t *testing.T) {
	for v, want := range map[Tenths]string{214: "21.4", 210: "21.0", -5: "-0.5", 0: "0.0", -123: "-12.3"} {
		got, _ := v.MarshalJSON()
		if string(got) != want {
			t.Errorf("%d → %s, want %s", v, got, want)
		}
	}
}

// Every payload the simulator writes passes the ingestor's validation (the contract round trip).
func TestPayloadsFollowTheContract(t *testing.T) {
	d, err := NewDevice("sim-vault-1", "Sim-1")
	if err != nil {
		t.Fatal(err)
	}
	now := time.Date(2026, 11, 20, 12, 0, 0, 120e6, time.UTC)
	for i := 1; i <= 200; i++ {
		raw, err := d.Next(now)
		if err != nil {
			t.Fatal(err)
		}
		tel, sampledAt, err := edge.ParseTelemetry(raw, "sim-vault-1")
		if err != nil {
			t.Fatalf("%s: %v", raw, err)
		}
		if tel.MessageID != uint32(i) || tel.BootID != d.BootID || !sampledAt.Equal(now) {
			t.Fatalf("%+v", tel)
		}
		if i == 1 && !strings.HasPrefix(string(raw), `{"node_id":"sim-vault-1","bunker_name":"Sim-1","timestamp":"2026-11-20T12:00:00.120Z","temperature":`) {
			t.Fatalf("field order: %s", raw)
		}
	}
	boot, _ := d.Boot(now)
	if _, err := edge.ParseEvent(boot, "sim-vault-1"); err != nil {
		t.Fatal(err)
	}
	health, _ := d.Health(now, 90*time.Second)
	if h, err := edge.ParseHealth(health, "sim-vault-1"); err != nil || h.UptimeS != 90 {
		t.Fatalf("%+v %v", h, err)
	}
	for tenths, want := range map[int]string{299: "OK", 300: "WARN", 349: "WARN", 350: "ALERT"} {
		if status(tenths) != want {
			t.Errorf("status(%d)", tenths)
		}
	}
}

type fakePublisher struct {
	mu     *sync.Mutex
	topics *[]string
	fail   string
}

func (p fakePublisher) Publish(topic string, _ []byte) error {
	if p.fail != "" && strings.HasSuffix(topic, p.fail) {
		return errors.New("broker gone")
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	*p.topics = append(*p.topics, topic)
	return nil
}
func (fakePublisher) Close() {}

func TestRunPublishesCountPerDevice(t *testing.T) {
	var mu sync.Mutex
	var topics []string
	dial := func(string) (Publisher, error) { return fakePublisher{mu: &mu, topics: &topics}, nil }
	cfg := Config{TopicPrefix: "eop/bunker", NodePrefix: "sim-vault-", Devices: 3, Period: time.Millisecond, Count: 10}
	sent, err := Run(context.Background(), cfg, dial, time.Now, quiet)
	if err != nil || sent != 30 {
		t.Fatalf("sent %d, %v", sent, err)
	}
	count := map[string]int{}
	for _, topic := range topics {
		count[topic]++
	}
	if count["eop/bunker/sim-vault-2/telemetry"] != 10 || count["eop/bunker/sim-vault-2/events"] != 1 || count["eop/bunker/sim-vault-2/health"] != 1 {
		t.Fatalf("%v", count)
	}
}

func TestRunReportsFailures(t *testing.T) {
	cfg := Config{TopicPrefix: "eop/bunker", NodePrefix: "sim-", Devices: 2, Period: time.Millisecond, Count: 5}
	if _, err := Run(context.Background(), cfg, func(string) (Publisher, error) { return nil, errors.New("refused") }, time.Now, quiet); err == nil {
		t.Fatal("dial failure hidden")
	}
	var mu sync.Mutex
	var topics []string
	for _, fail := range []string{"events", "telemetry"} {
		dial := func(string) (Publisher, error) { return fakePublisher{mu: &mu, topics: &topics, fail: fail}, nil }
		if _, err := Run(context.Background(), cfg, dial, time.Now, quiet); err == nil {
			t.Fatalf("%s failure hidden", fail)
		}
	}
	healthFails := func(string) (Publisher, error) { return fakePublisher{mu: &mu, topics: &topics, fail: "health"}, nil }
	if _, err := Run(context.Background(), Config{TopicPrefix: "p", NodePrefix: "n-", Devices: 1, Period: time.Millisecond, Count: 10}, healthFails, time.Now, quiet); err == nil {
		t.Fatal("health failure hidden")
	}
}

func TestRunUntilCancelled(t *testing.T) {
	var mu sync.Mutex
	var topics []string
	dial := func(string) (Publisher, error) { return fakePublisher{mu: &mu, topics: &topics}, nil }
	ctx, cancel := context.WithTimeout(context.Background(), 50*time.Millisecond)
	defer cancel()
	sent, err := Run(ctx, Config{TopicPrefix: "p", NodePrefix: "n-", Devices: 1, Period: 5 * time.Millisecond}, dial, time.Now, quiet)
	if err != nil || sent < 2 {
		t.Fatalf("sent %d, %v", sent, err)
	}
}

// Through a real broker: 10 samples arrive, in order, each acknowledged.
func TestMQTTDialPublishesToABroker(t *testing.T) {
	l, _ := net.Listen("tcp", "127.0.0.1:0")
	addr := l.Addr().String()
	_ = l.Close()
	server := mochi.New(&mochi.Options{InlineClient: true})
	_ = server.AddHook(new(auth.AllowHook), nil)
	_ = server.AddListener(listeners.NewTCP(listeners.Config{ID: "tcp", Address: addr}))
	go func() { _ = server.Serve() }()
	defer func() { _ = server.Close() }()

	var mu sync.Mutex
	var ids []string
	if err := server.Subscribe("eop/bunker/+/telemetry", 1, func(_ *mochi.Client, _ packets.Subscription, pk packets.Packet) {
		tel, _, err := edge.ParseTelemetry(pk.Payload, "sim-vault-1")
		mu.Lock()
		defer mu.Unlock()
		if err == nil {
			ids = append(ids, string(rune('0'+tel.MessageID%10)))
		}
	}); err != nil {
		t.Fatal(err)
	}
	cfg := Config{Broker: "tcp://" + addr, TopicPrefix: "eop/bunker", NodePrefix: "sim-vault-", Devices: 1,
		Period: time.Millisecond, Count: 10, Timeout: 5 * time.Second}
	sent, err := Run(context.Background(), cfg, MQTTDial(cfg), time.Now, quiet)
	if err != nil || sent != 10 {
		t.Fatalf("sent %d, %v", sent, err)
	}
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		mu.Lock()
		got := strings.Join(ids, "")
		mu.Unlock()
		if got == "1234567890" {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatalf("broker got message ids %v", ids)
}

func TestMQTTDialFailsWithoutABroker(t *testing.T) {
	cfg := Config{Broker: "tcp://127.0.0.1:1", Timeout: 2 * time.Second}
	if _, err := MQTTDial(cfg)("sim-1"); err == nil {
		t.Fatal("connected to nothing")
	}
}
