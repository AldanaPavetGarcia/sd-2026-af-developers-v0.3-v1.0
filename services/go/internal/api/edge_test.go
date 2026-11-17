package api

import (
	"context"
	"errors"
	"net/http"
	"strings"
	"testing"
	"time"

	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/edge"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/eopclient"
)

type fakeEdge struct {
	snapshot edge.Snapshot
	ok       bool
	err      error
}

func (f fakeEdge) EdgeStatus(context.Context) (edge.Snapshot, bool, error) {
	return f.snapshot, f.ok, f.err
}

func withEdge(reader EdgeStatusReader) func(*Options) {
	return func(o *Options) { o.Edge = reader }
}

// US-404 AC7.
func TestEdgeStatusReportsTheSnapshot(t *testing.T) {
	seen := time.Date(2026, 11, 20, 12, 0, 0, 0, time.UTC)
	reader := fakeEdge{ok: true, snapshot: edge.Snapshot{State: edge.StateConnected, WindowSeconds: 60, Received: 600,
		Forwarded: 598, Duplicates: 2, Devices: []edge.Device{{NodeID: "vault-101", LastSeen: seen, Connected: true}}}}
	h := newHarness(t, &fakeStore{}, withEdge(reader))
	rec := h.do(http.MethodGet, "/v1/edge/status")
	if rec.Code != http.StatusOK {
		t.Fatalf("status %d: %s", rec.Code, rec.Body)
	}
	data := decode(t, rec)["data"].(map[string]any)
	if data["state"] != "connected" || data["received"].(float64) != 600 || data["forwarded"].(float64) != 598 {
		t.Fatalf("%v", data)
	}
	devices := data["devices"].([]any)
	if len(devices) != 1 || devices[0].(map[string]any)["node_id"] != "vault-101" {
		t.Fatalf("%v", devices)
	}
}

func TestEdgeStatusWithoutASnapshotIsDisconnected(t *testing.T) {
	for name, reader := range map[string]EdgeStatusReader{"no snapshot": fakeEdge{}, "no reader": nil} {
		t.Run(name, func(t *testing.T) {
			h := newHarness(t, &fakeStore{}, withEdge(reader))
			rec := h.do(http.MethodGet, "/v1/edge/status")
			data := decode(t, rec)["data"].(map[string]any)
			if rec.Code != http.StatusOK || data["state"] != "disconnected" || data["window_seconds"].(float64) != 60 ||
				data["received"].(float64) != 0 || len(data["devices"].([]any)) != 0 {
				t.Fatalf("%d %v", rec.Code, data)
			}
		})
	}
}

func TestEdgeStatusStoreDownIs503(t *testing.T) {
	h := newHarness(t, &fakeStore{}, withEdge(fakeEdge{err: errors.New("redis down")}))
	rec := h.do(http.MethodGet, "/v1/edge/status")
	if rec.Code != http.StatusServiceUnavailable || !strings.Contains(rec.Body.String(), CodeUpstreamUnavailable) {
		t.Fatalf("%d %s", rec.Code, rec.Body)
	}
}

// US-404 AC8: edge nodes carry their newest sample; TCP nodes are unchanged.
func TestNodesCarryEdgeTelemetry(t *testing.T) {
	store := &fakeStore{nodes: []eopclient.Node{
		sampleNodes[0],
		{NodeID: "vault-101", BunkerName: "Vault-101", IPAddress: "0.0.0.0", Status: "ONLINE", LastSeen: time.Unix(1793628187, 0),
			Source: eopclient.SourceEdge, Telemetry: &eopclient.EdgeTelemetry{Temperature: 21.4, DeviceStatus: "WARN",
				SampledAt: time.UnixMilli(1793628187120)}},
	}}
	h := newHarness(t, store)
	rec := h.do(http.MethodGet, "/v1/nodes")
	nodes := decode(t, rec)["data"].([]any)
	tcp, device := nodes[0].(map[string]any), nodes[1].(map[string]any)
	if tcp["source"] != "tcp" || tcp["telemetry"] != nil {
		t.Fatalf("tcp node %v", tcp)
	}
	tel := device["telemetry"].(map[string]any)
	if device["source"] != "edge" || tel["temperature"].(float64) != 21.4 || tel["status"] != "WARN" ||
		tel["sampled_at"] != "2026-11-02T14:03:07.120Z" {
		t.Fatalf("edge node %v", device)
	}
	if rec := h.do(http.MethodGet, "/v1/nodes/vault-101"); rec.Code != http.StatusOK {
		t.Fatalf("%d", rec.Code)
	}
}
