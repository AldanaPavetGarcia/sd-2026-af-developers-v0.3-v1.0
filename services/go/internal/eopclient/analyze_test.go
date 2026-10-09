package eopclient

import (
	"context"
	"encoding/json"
	"errors"
	"strings"
	"testing"
	"time"

	sdktrace "go.opentelemetry.io/otel/sdk/trace"
)

var smallGraph = AnalyzeRequest{
	Nodes:      []string{"A", "B"},
	Edges:      []Edge{{From: "A", To: "B", Weight: 1}},
	Algorithms: []string{"connected_components"},
}

func ackFor(requestID, result string) string {
	return `{"type":"RESPONSE","status":"ACK","request_id":"` + requestID + `","result":` + result + "}\n"
}

func TestAnalyzeGraphSendsTheEnvelopeAndReturnsTheResult(t *testing.T) {
	var sent map[string]any
	server := newFakeServer(t, func(request string) string {
		if err := json.Unmarshal([]byte(request), &sent); err != nil {
			return "ERROR_BAD_REQUEST desc=not json\n"
		}
		return ackFor("job-1", `{"graph_size":{"nodes":2,"edges":1},"algorithms":{"connected_components":[["A","B"]]}}`)
	})
	client := newTestClient(t, testOptions(server.addr()))

	provider := sdktrace.NewTracerProvider()
	defer func() { _ = provider.Shutdown(context.Background()) }()
	ctx, span := provider.Tracer("test").Start(context.Background(), "analyze")
	defer span.End()

	result, err := client.AnalyzeGraph(ctx, "job-1", smallGraph)
	if err != nil {
		t.Fatalf("AnalyzeGraph: %v", err)
	}
	if !strings.Contains(string(result), `"connected_components":[["A","B"]]`) {
		t.Fatalf("result = %s", result)
	}
	payload := sent["payload"].(map[string]any)
	if sent["type"] != "ANALYZE_GRAPH" || sent["request_id"] != "job-1" || len(payload["nodes"].([]any)) != 2 {
		t.Fatalf("envelope = %v", sent)
	}
	if tp, _ := sent["traceparent"].(string); !strings.Contains(tp, span.SpanContext().TraceID().String()) {
		t.Fatalf("traceparent = %q, want the caller's trace", tp)
	}
}

func TestAnalyzeGraphErrors(t *testing.T) {
	tests := map[string]struct {
		answer string
		check  func(error) bool
	}{
		"no engine (text)": {"ERROR_SERVICE_UNAVAILABLE desc=HPC engine unavailable\n", IsEngineUnavailable},
		"engine timeout":   {"ERROR_HPC_TIMEOUT desc=HPC engine timeout\n", IsEngineUnavailable},
		"engine error": {`{"type":"RESPONSE","status":"ERROR","request_id":"r","error_code":"ERROR_INVALID_GRAPH","message":"bad edge","field":"payload.edges[0].to"}` + "\n", func(err error) bool {
			var aerr *AnalysisError
			return errors.As(err, &aerr) && aerr.Field == "payload.edges[0].to" && !IsEngineUnavailable(err) && strings.Contains(err.Error(), "ERROR_INVALID_GRAPH")
		}},
		"engine unavailable (json)": {`{"type":"RESPONSE","status":"ERROR","error_code":"ERROR_SERVICE_UNAVAILABLE","message":"x"}` + "\n", IsEngineUnavailable},
		"not json":                  {"OK\n", isMalformed},
		"invalid json":              {"{not json\n", isMalformed},
		"wrong type":                {`{"type":"REQUEST","status":"ACK","result":{}}` + "\n", isMalformed},
		"other request":             {ackFor("someone-else", "{}"), isMalformed},
		"ack without result":        {`{"type":"RESPONSE","status":"ACK","request_id":"r"}` + "\n", isMalformed},
		"error without code":        {`{"type":"RESPONSE","status":"ERROR","request_id":"r"}` + "\n", isMalformed},
		"unknown status":            {`{"type":"RESPONSE","status":"MAYBE","request_id":"r"}` + "\n", isMalformed},
	}
	for name, tt := range tests {
		t.Run(name, func(t *testing.T) {
			server := newFakeServer(t, func(string) string { return tt.answer })
			_, err := newTestClient(t, testOptions(server.addr())).AnalyzeGraph(context.Background(), "r", smallGraph)
			if err == nil || !tt.check(err) {
				t.Fatalf("error = %v", err)
			}
		})
	}
}

func isMalformed(err error) bool {
	var malformed *MalformedResponseError
	return errors.As(err, &malformed)
}

func TestAnalyzeGraphReadsAnswersLargerThanTheLineBuffer(t *testing.T) {
	big := `{"centrality":{"x":"` + strings.Repeat("a", 50_000) + `"}}`
	server := newFakeServer(t, func(string) string { return ackFor("r", big) })
	opts := testOptions(server.addr())
	opts.MaxLineBytes = 1024 // far smaller than the answer: the long-line reader must loop
	result, err := newTestClient(t, opts).AnalyzeGraph(context.Background(), "r", smallGraph)
	if err != nil || len(result) != len(big) {
		t.Fatalf("len(result) = %d, err = %v", len(result), err)
	}
}

func TestAnalyzeGraphRejectsAnswersOverTheLimit(t *testing.T) {
	server := newFakeServer(t, func(string) string { return ackFor("r", `{"x":"`+strings.Repeat("a", 10_000)+`"}`) })
	opts := testOptions(server.addr())
	opts.MaxLineBytes = 1024
	opts.MaxAnalyzeResponseBytes = 4096
	if _, err := newTestClient(t, opts).AnalyzeGraph(context.Background(), "r", smallGraph); !isMalformed(err) {
		t.Fatalf("error = %v, want MalformedResponseError", err)
	}
}

func TestAnalyzeGraphRejectsUnsafeRequestIDs(t *testing.T) {
	server := newFakeServer(t, protocolHandler)
	client := newTestClient(t, testOptions(server.addr()))
	for _, id := range []string{"", "a\nLIST_NODES"} {
		if _, err := client.AnalyzeGraph(context.Background(), id, smallGraph); !errors.Is(err, ErrInvalidArgument) {
			t.Fatalf("request_id %q: error = %v", id, err)
		}
	}
	if server.accepted.Load() != 0 {
		t.Fatal("an unsafe request_id reached the server")
	}
}

func TestAnalyzeGraphIsNeverRetried(t *testing.T) {
	server := newFakeServer(t, protocolHandler)
	client := newTestClient(t, testOptions(server.addr()))
	if err := client.Heartbeat(context.Background()); err != nil {
		t.Fatal(err)
	}
	server.dropConnections()
	if _, err := client.AnalyzeGraph(context.Background(), "r", smallGraph); err == nil {
		t.Fatal("AnalyzeGraph succeeded on a dropped connection")
	}
	analyzeRequests := 0
	for _, r := range server.recorded() {
		if strings.Contains(r, "ANALYZE_GRAPH") {
			analyzeRequests++
		}
	}
	if server.accepted.Load() != 1 || analyzeRequests > 1 {
		t.Fatalf("connections = %d, analyze requests = %d: an analysis must not be sent twice", server.accepted.Load(), analyzeRequests)
	}
}

func TestCancellationInterruptsABlockedRead(t *testing.T) {
	release := make(chan struct{})
	server := newFakeServer(t, func(string) string {
		<-release
		return ""
	})
	// Registered after the server, so it runs first: the server's cleanup waits for its handler.
	t.Cleanup(func() { close(release) })
	opts := testOptions(server.addr())
	opts.AnalyzeTimeout = time.Minute
	client := newTestClient(t, opts)

	ctx, cancel := context.WithCancel(context.Background())
	go func() {
		time.Sleep(50 * time.Millisecond)
		cancel()
	}()
	start := time.Now()
	_, err := client.AnalyzeGraph(ctx, "r", smallGraph)
	if !errors.Is(err, context.Canceled) {
		t.Fatalf("error = %v, want context.Canceled", err)
	}
	if elapsed := time.Since(start); elapsed > 5*time.Second {
		t.Fatalf("cancellation took %v; the read was not interrupted", elapsed)
	}
}

func TestDeadlineIsReportedAsDeadlineExceeded(t *testing.T) {
	release := make(chan struct{})
	server := newFakeServer(t, func(string) string {
		<-release
		return ""
	})
	// Registered after the server, so it runs first: the server's cleanup waits for its handler.
	t.Cleanup(func() { close(release) })
	client := newTestClient(t, testOptions(server.addr()))
	ctx, cancel := context.WithTimeout(context.Background(), 100*time.Millisecond)
	defer cancel()
	if _, err := client.AnalyzeGraph(ctx, "r", smallGraph); !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("error = %v, want context.DeadlineExceeded", err)
	}
}

func TestAnalyzeGraphDoesNotReuseItsConnection(t *testing.T) {
	server := newFakeServer(t, func(request string) string {
		if strings.HasPrefix(request, "{") {
			return ackFor("r", "{}")
		}
		return protocolHandler(request)
	})
	client := newTestClient(t, testOptions(server.addr()))
	if _, err := client.AnalyzeGraph(context.Background(), "r", smallGraph); err != nil {
		t.Fatal(err)
	}
	if err := client.Heartbeat(context.Background()); err != nil {
		t.Fatal(err)
	}
	if got := server.accepted.Load(); got != 2 {
		t.Fatalf("server accepted %d connections, want 2 (analysis connection closed after use)", got)
	}
}
