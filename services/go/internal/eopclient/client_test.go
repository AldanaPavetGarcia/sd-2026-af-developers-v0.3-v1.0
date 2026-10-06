package eopclient

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"

	sdktrace "go.opentelemetry.io/otel/sdk/trace"
)

const twoNodes = "LIST_NODES count=2\n" +
	"node_id=alpha bunker_name=A ip_address=10.0.0.1 capacity=5 status=ONLINE last_seen=100\n" +
	"node_id=beta bunker_name=B ip_address=10.0.0.2 capacity=7 status=OFFLINE last_seen=200\n" +
	"END_LIST\n"

func protocolHandler(request string) string {
	command, _, _ := strings.Cut(request, " ")
	switch command {
	case "LIST_NODES":
		return twoNodes
	case "QUERY_NODE":
		if strings.Contains(request, "node_id=alpha") {
			return "node_id=alpha bunker_name=A ip_address=10.0.0.1 capacity=5 status=ONLINE last_seen=100\n"
		}
		return "ERROR_NOT_FOUND node_id=x desc=Node not found\n"
	case "HEARTBEAT":
		return "ACK HEARTBEAT\n"
	}
	return "ERROR_BAD_REQUEST desc=Unknown command\n"
}

func TestOptionsValidate(t *testing.T) {
	valid := testOptions("127.0.0.1:1")
	if err := valid.Validate(); err != nil {
		t.Fatalf("valid options rejected: %v", err)
	}
	mutations := map[string]func(*Options){
		"address":   func(o *Options) { o.Address = "" },
		"timeout":   func(o *Options) { o.IdleTimeout = 0 },
		"max conns": func(o *Options) { o.MaxConns = 0 },
		"limits":    func(o *Options) { o.MaxLineBytes = 0 },
	}
	for name, mutate := range mutations {
		opts := valid
		mutate(&opts)
		if _, err := New(opts); err == nil {
			t.Errorf("%s: New accepted invalid options", name)
		}
	}
}

func TestListNodes(t *testing.T) {
	server := newFakeServer(t, protocolHandler)
	client := newTestClient(t, testOptions(server.addr()))

	nodes, err := client.ListNodes(context.Background())
	if err != nil {
		t.Fatalf("ListNodes: %v", err)
	}
	if len(nodes) != 2 || nodes[0].NodeID != "alpha" || nodes[1].Status != "OFFLINE" || nodes[1].LastSeen.Unix() != 200 {
		t.Fatalf("ListNodes = %+v", nodes)
	}
}

func TestListNodesEmptyRegistry(t *testing.T) {
	server := newFakeServer(t, func(string) string { return "LIST_NODES count=0\nEND_LIST\n" })
	client := newTestClient(t, testOptions(server.addr()))

	nodes, err := client.ListNodes(context.Background())
	if err != nil || len(nodes) != 0 {
		t.Fatalf("ListNodes = %v, %v; want empty, nil", nodes, err)
	}
}

func TestListNodesRejectsMalformedAnswers(t *testing.T) {
	tests := map[string]string{
		"missing END_LIST":   "LIST_NODES count=1\nnode_id=a bunker_name=b ip_address=c capacity=1 status=ONLINE last_seen=1\nnode_id=x\n",
		"count over limit":   "LIST_NODES count=101\n",
		"bad record":         "LIST_NODES count=1\nnode_id=a\nEND_LIST\n",
		"server error":       "ERROR_BAD_REQUEST desc=nope\n",
		"wrong header":       "QUERY_NODE count=1\n",
		"connection dropped": "LIST_NODES count=2\nnode_id=a bunker_name=b ip_address=c capacity=1 status=ONLINE last_seen=1\n",
	}
	for name, answer := range tests {
		t.Run(name, func(t *testing.T) {
			server := newFakeServer(t, func(string) string { return answer })
			opts := testOptions(server.addr())
			opts.RequestTimeout = 200 * time.Millisecond
			client := newTestClient(t, opts)
			if _, err := client.ListNodes(context.Background()); err == nil {
				t.Fatal("ListNodes accepted a malformed answer")
			}
		})
	}
}

func TestQueryNode(t *testing.T) {
	server := newFakeServer(t, protocolHandler)
	client := newTestClient(t, testOptions(server.addr()))

	node, err := client.QueryNode(context.Background(), "alpha")
	if err != nil || node.NodeID != "alpha" || node.Capacity != 5 {
		t.Fatalf("QueryNode = %+v, %v", node, err)
	}

	_, err = client.QueryNode(context.Background(), "ghost")
	if !errors.Is(err, ErrNotFound) {
		t.Fatalf("QueryNode(ghost) error = %v, want ErrNotFound", err)
	}
	// An ERROR_* answer leaves the connection usable: both calls share one connection.
	if got := server.accepted.Load(); got != 1 {
		t.Fatalf("server accepted %d connections, want 1", got)
	}
}

func TestQueryNodeRejectsUnsafeIDBeforeSending(t *testing.T) {
	server := newFakeServer(t, protocolHandler)
	client := newTestClient(t, testOptions(server.addr()))

	_, err := client.QueryNode(context.Background(), "alpha\nLIST_NODES")
	if !errors.Is(err, ErrInvalidArgument) {
		t.Fatalf("error = %v, want ErrInvalidArgument", err)
	}
	if len(server.recorded()) != 0 || server.accepted.Load() != 0 {
		t.Fatal("an unsafe node_id must not reach the server")
	}
}

func TestQueryNodeMalformedRecord(t *testing.T) {
	server := newFakeServer(t, func(string) string { return "node_id=alpha\n" })
	client := newTestClient(t, testOptions(server.addr()))
	var malformed *MalformedResponseError
	if _, err := client.QueryNode(context.Background(), "alpha"); !errors.As(err, &malformed) {
		t.Fatalf("error = %v, want MalformedResponseError", err)
	}
}

func TestHeartbeat(t *testing.T) {
	server := newFakeServer(t, protocolHandler)
	client := newTestClient(t, testOptions(server.addr()))
	if err := client.Heartbeat(context.Background()); err != nil {
		t.Fatalf("Heartbeat: %v", err)
	}

	for name, answer := range map[string]string{"unexpected": "ACK SOMETHING\n", "error": "ERROR_BAD_REQUEST desc=x\n"} {
		t.Run(name, func(t *testing.T) {
			bad := newFakeServer(t, func(string) string { return answer })
			if err := newTestClient(t, testOptions(bad.addr())).Heartbeat(context.Background()); err == nil {
				t.Fatal("Heartbeat accepted a wrong answer")
			}
		})
	}
}

func TestDialFailuresAreUnavailable(t *testing.T) {
	server := newFakeServer(t, protocolHandler)
	addr := server.addr()
	server.close()

	client := newTestClient(t, testOptions(addr))
	err := client.Heartbeat(context.Background())
	if !IsUnavailable(err) {
		t.Fatalf("error = %v, want unavailable", err)
	}
	if !strings.Contains(err.Error(), "unavailable") {
		t.Fatalf("Error() = %q", err.Error())
	}
}

func TestMissingBannerIsRejected(t *testing.T) {
	server := newFakeServerWithBanner(t, "HELLO", protocolHandler)
	client := newTestClient(t, testOptions(server.addr()))
	var malformed *MalformedResponseError
	if err := client.Heartbeat(context.Background()); !errors.As(err, &malformed) {
		t.Fatalf("error = %v, want MalformedResponseError", err)
	}
}

func TestSilentServerTimesOutAsUnavailable(t *testing.T) {
	server := newFakeServerWithBanner(t, "", protocolHandler) // never greets
	opts := testOptions(server.addr())
	opts.DialTimeout = 100 * time.Millisecond
	client := newTestClient(t, opts)
	if err := client.Heartbeat(context.Background()); !IsUnavailable(err) {
		t.Fatalf("error = %v, want unavailable", err)
	}
}

func TestOverlongLineIsRejected(t *testing.T) {
	server := newFakeServerWithBanner(t, "WELCOME", func(string) string { return strings.Repeat("x", 200) + "\n" })
	opts := testOptions(server.addr())
	opts.MaxLineBytes = 64
	client := newTestClient(t, opts)
	var malformed *MalformedResponseError
	if err := client.Heartbeat(context.Background()); !errors.As(err, &malformed) {
		t.Fatalf("error = %v, want MalformedResponseError", err)
	}
}

func TestPoolReusesConnections(t *testing.T) {
	server := newFakeServer(t, protocolHandler)
	client := newTestClient(t, testOptions(server.addr()))
	for i := 0; i < 5; i++ {
		if err := client.Heartbeat(context.Background()); err != nil {
			t.Fatalf("Heartbeat %d: %v", i, err)
		}
	}
	if got := server.accepted.Load(); got != 1 {
		t.Fatalf("server accepted %d connections, want 1", got)
	}
}

func TestPoolDropsConnectionsIdleTooLong(t *testing.T) {
	server := newFakeServer(t, protocolHandler)
	client := newTestClient(t, testOptions(server.addr()))
	now := time.Now()
	client.now = func() time.Time { return now }

	if err := client.Heartbeat(context.Background()); err != nil {
		t.Fatal(err)
	}
	now = now.Add(2 * time.Minute) // longer than IdleTimeout
	if err := client.Heartbeat(context.Background()); err != nil {
		t.Fatal(err)
	}
	if got := server.accepted.Load(); got != 2 {
		t.Fatalf("server accepted %d connections, want 2 (stale one replaced)", got)
	}
}

func TestRetriesOnceWhenPooledConnectionWasClosed(t *testing.T) {
	server := newFakeServer(t, protocolHandler)
	client := newTestClient(t, testOptions(server.addr()))
	if err := client.Heartbeat(context.Background()); err != nil {
		t.Fatal(err)
	}
	server.dropConnections() // like the server's idle timeout

	nodes, err := client.ListNodes(context.Background())
	if err != nil || len(nodes) != 2 {
		t.Fatalf("ListNodes after drop = %v, %v", nodes, err)
	}
	if got := server.accepted.Load(); got != 2 {
		t.Fatalf("server accepted %d connections, want 2", got)
	}
}

func TestDoesNotRetryOnFreshConnection(t *testing.T) {
	server := newFakeServer(t, func(string) string { return "" }) // closes without answering
	client := newTestClient(t, testOptions(server.addr()))
	if err := client.Heartbeat(context.Background()); err == nil {
		t.Fatal("Heartbeat succeeded against a server that never answers")
	}
	if got := server.accepted.Load(); got != 1 {
		t.Fatalf("server accepted %d connections, want 1 (no retry on a fresh connection)", got)
	}
}

func TestPoolBoundsOpenConnections(t *testing.T) {
	release := make(chan struct{})
	server := newFakeServer(t, func(string) string {
		<-release
		return "ACK HEARTBEAT\n"
	})
	opts := testOptions(server.addr())
	opts.MaxConns = 1
	client := newTestClient(t, opts)

	done := make(chan error, 1)
	go func() { done <- client.Heartbeat(context.Background()) }()
	time.Sleep(50 * time.Millisecond) // first call holds the only slot

	ctx, cancel := context.WithTimeout(context.Background(), 50*time.Millisecond)
	defer cancel()
	if err := client.Heartbeat(ctx); !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("second call error = %v, want deadline exceeded while the pool is full", err)
	}
	close(release)
	if err := <-done; err != nil {
		t.Fatalf("first call: %v", err)
	}
	if got := server.accepted.Load(); got != 1 {
		t.Fatalf("server accepted %d connections, want 1", got)
	}
}

func TestClosedClientRefusesCommands(t *testing.T) {
	server := newFakeServer(t, protocolHandler)
	client := newTestClient(t, testOptions(server.addr()))
	if err := client.Heartbeat(context.Background()); err != nil {
		t.Fatal(err)
	}
	if err := client.Close(); err != nil {
		t.Fatal(err)
	}
	if err := client.Heartbeat(context.Background()); err == nil {
		t.Fatal("closed client sent a command")
	}
}

func TestTraceparentIsAppendedToTextCommands(t *testing.T) {
	server := newFakeServer(t, protocolHandler)
	client := newTestClient(t, testOptions(server.addr()))

	provider := sdktrace.NewTracerProvider()
	defer func() { _ = provider.Shutdown(context.Background()) }()
	ctx, span := provider.Tracer("test").Start(context.Background(), "request")
	defer span.End()

	if _, err := client.QueryNode(ctx, "alpha"); err != nil {
		t.Fatal(err)
	}
	if err := client.Heartbeat(context.Background()); err != nil {
		t.Fatal(err)
	}
	requests := server.recorded()
	want := "QUERY_NODE node_id=alpha traceparent=00-" + span.SpanContext().TraceID().String() + "-"
	if !strings.HasPrefix(requests[0], want) {
		t.Fatalf("request = %q, want prefix %q", requests[0], want)
	}
	if requests[1] != "HEARTBEAT" {
		t.Fatalf("request without a span = %q, want no traceparent", requests[1])
	}
}

func TestIsUnavailable(t *testing.T) {
	if IsUnavailable(&ProtocolError{Code: CodeNotFound}) {
		t.Fatal("a server answer is not unavailability")
	}
	if !IsUnavailable(context.DeadlineExceeded) {
		t.Fatal("a deadline is unavailability")
	}
	if !errors.Is(&UnavailableError{Err: context.Canceled}, context.Canceled) {
		t.Fatal("UnavailableError must unwrap")
	}
}
