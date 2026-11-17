package edge

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/alicebob/miniredis/v2"
	"github.com/redis/go-redis/v9"
)

// The example of docs/edge-telemetry-protocol-v1.0.md §2.1, as the firmware encodes it.
const telemetryExample = `{"node_id":"vault-101","bunker_name":"Vault-101","timestamp":"2026-11-02T14:03:07.120Z","temperature":21.4,"status":"OK","message_id":4182,"boot_id":"9f3c27a1"}`

func TestParseTelemetry(t *testing.T) {
	tel, sampledAt, err := ParseTelemetry([]byte(telemetryExample), "vault-101")
	if err != nil {
		t.Fatal(err)
	}
	if tel.MessageID != 4182 || tel.Temperature != 21.4 || tel.BootID != "9f3c27a1" {
		t.Fatalf("%+v", tel)
	}
	if want := time.Date(2026, 11, 2, 14, 3, 7, 120e6, time.UTC); !sampledAt.Equal(want) {
		t.Fatalf("sampledAt = %v", sampledAt)
	}
}

func TestParseTelemetryRejects(t *testing.T) {
	cases := map[string]string{
		"not json":      `{`,
		"unknown field": strings.Replace(telemetryExample, `"boot_id"`, `"extra":1,"boot_id"`, 1),
		"trailing data": telemetryExample + `{}`,
		"node id":       strings.Replace(telemetryExample, `"vault-101"`, `"Vault 101"`, 1),
		"engine id":     strings.Replace(telemetryExample, `"vault-101"`, `"hpc-engine-x"`, 1),
		"bunker name":   strings.Replace(telemetryExample, `"Vault-101"`, `"Vault 101"`, 1),
		"boot id":       strings.Replace(telemetryExample, `9f3c27a1`, `9F3C27A1`, 1),
		"status":        strings.Replace(telemetryExample, `"OK"`, `"HOT"`, 1),
		"temperature":   strings.Replace(telemetryExample, `21.4`, `151`, 1),
		"message id":    strings.Replace(telemetryExample, `4182`, `0`, 1),
		"timestamp":     strings.Replace(telemetryExample, `14:03:07.120Z`, `14:03:07Z`, 1),
	}
	for name, raw := range cases {
		t.Run(name, func(t *testing.T) {
			if _, _, err := ParseTelemetry([]byte(raw), "vault-101"); !errors.Is(err, ErrInvalid) {
				t.Fatalf("err = %v", err)
			}
		})
	}
	if _, _, err := ParseTelemetry([]byte(telemetryExample), "vault-102"); !errors.Is(err, ErrInvalid) {
		t.Fatalf("payload of another device's topic accepted: %v", err)
	}
}

func TestParseEvent(t *testing.T) {
	boot := `{"node_id":"vault-101","boot_id":"9f3c27a1","timestamp":"2026-11-02T14:03:07.120Z","event":"BOOT"}`
	rec := `{"node_id":"vault-101","boot_id":"9f3c27a1","timestamp":"2026-11-02T14:03:07.120Z","event":"RECONNECTED","delivered":37}`
	if e, err := ParseEvent([]byte(boot), "vault-101"); err != nil || e.Event != "BOOT" || e.Delivered != nil {
		t.Fatalf("%+v %v", e, err)
	}
	if e, err := ParseEvent([]byte(rec), "vault-101"); err != nil || *e.Delivered != 37 {
		t.Fatalf("%+v %v", e, err)
	}
	for _, raw := range []string{
		strings.Replace(boot, `"BOOT"`, `"BOOT","delivered":1`, 1),
		strings.Replace(rec, `,"delivered":37`, ``, 1),
		strings.Replace(boot, `BOOT`, `CRASH`, 1),
		strings.Replace(boot, `14:03:07.120Z`, `x`, 1),
		strings.Replace(boot, `9f3c27a1`, `x`, 1),
		`[]`,
	} {
		if _, err := ParseEvent([]byte(raw), "vault-101"); !errors.Is(err, ErrInvalid) {
			t.Errorf("%s accepted: %v", raw, err)
		}
	}
}

func TestParseHealth(t *testing.T) {
	raw := `{"node_id":"vault-101","boot_id":"9f3c27a1","timestamp":"2026-11-02T14:03:07.120Z","uptime_s":3600,"restarts":{"sensor":0,"mqtt":1},"buffered":2,"dropped":5}`
	h, err := ParseHealth([]byte(raw), "vault-101")
	if err != nil || h.Restarts["mqtt"] != 1 || h.Dropped != 5 {
		t.Fatalf("%+v %v", h, err)
	}
	for _, bad := range []string{
		strings.Replace(raw, `,"restarts":{"sensor":0,"mqtt":1}`, ``, 1),
		strings.Replace(raw, `14:03:07.120Z`, `x`, 1),
		strings.Replace(raw, `"vault-101"`, `"x y"`, 1),
		`"x"`,
	} {
		if _, err := ParseHealth([]byte(bad), "vault-101"); !errors.Is(err, ErrInvalid) {
			t.Errorf("%s accepted: %v", bad, err)
		}
	}
}

func TestTopics(t *testing.T) {
	topic := Topic("eop/bunker", "vault-7", KindHealth)
	if topic != "eop/bunker/vault-7/health" {
		t.Fatal(topic)
	}
	node, kind, err := SplitTopic("eop/bunker", topic)
	if err != nil || node != "vault-7" || kind != KindHealth {
		t.Fatalf("%s %s %v", node, kind, err)
	}
	for _, bad := range []string{"other/vault-7/health", "eop/bunker/vault-7", "eop/bunker//health", "eop/bunker/a/b/c"} {
		if _, _, err := SplitTopic("eop/bunker", bad); !errors.Is(err, ErrInvalid) {
			t.Errorf("%s accepted", bad)
		}
	}
	at := time.Date(2026, 11, 2, 14, 3, 7, 120e6, time.FixedZone("ART", -3*3600))
	if FormatTimestamp(at) != "2026-11-02T17:03:07.120Z" {
		t.Fatal(FormatTimestamp(at))
	}
}

func TestSnapshotInRedis(t *testing.T) {
	mr := miniredis.RunT(t)
	rdb := redis.NewClient(&redis.Options{Addr: mr.Addr()})
	ctx := context.Background()

	if _, ok, err := ReadSnapshot(ctx, rdb); ok || err != nil {
		t.Fatalf("empty Redis: ok=%v err=%v", ok, err)
	}
	s := Snapshot{State: StateConnected, WindowSeconds: 60, Received: 10, Forwarded: 9, Duplicates: 1}
	if err := WriteSnapshot(ctx, rdb, s, 5*time.Second); err != nil {
		t.Fatal(err)
	}
	got, ok, err := ReadSnapshot(ctx, rdb)
	if err != nil || !ok || got.Received != 10 || got.Devices == nil {
		t.Fatalf("%+v %v %v", got, ok, err)
	}
	mr.FastForward(6 * time.Second)
	if _, ok, _ := ReadSnapshot(ctx, rdb); ok {
		t.Fatal("the snapshot must expire when the ingestor stops writing")
	}
	_ = mr.Set(StatusKey, "{")
	if _, _, err := ReadSnapshot(ctx, rdb); err == nil {
		t.Fatal("garbage accepted")
	}
	mr.Close()
	if _, _, err := ReadSnapshot(ctx, rdb); err == nil {
		t.Fatal("Redis down must be an error, not a missing snapshot")
	}
	if d := Disconnected(time.Minute); d.State != StateDisconnected || d.WindowSeconds != 60 || d.Devices == nil {
		t.Fatalf("%+v", d)
	}
}

func TestWindow(t *testing.T) {
	w := NewWindow(time.Minute)
	t0 := time.Unix(1_800_000_000, 0)
	w.Add(CountReceived, t0.Add(-2*time.Minute)) // outside the window
	for i := 0; i < 5; i++ {
		w.Add(CountReceived, t0.Add(-time.Duration(i)*time.Second))
	}
	w.Add(CountForwarded, t0)
	w.Add(CountDuplicates, t0)
	w.Add(CountInvalid, t0)
	w.Seen("vault-2", t0.Add(-30*time.Second))
	w.Seen("vault-1", t0)
	w.Seen("vault-1", t0.Add(-time.Second)) // older: ignored

	s := w.Snapshot(t0, StateReconnecting, 10*time.Second)
	if s.Received != 5 || s.Forwarded != 1 || s.Duplicates != 1 || s.Invalid != 1 || s.State != StateReconnecting {
		t.Fatalf("%+v", s)
	}
	if len(s.Devices) != 2 || s.Devices[0].NodeID != "vault-1" || !s.Devices[0].Connected || s.Devices[1].Connected {
		t.Fatalf("devices %+v", s.Devices)
	}
}
