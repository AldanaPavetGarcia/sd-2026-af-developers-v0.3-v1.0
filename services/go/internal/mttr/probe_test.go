package mttr

import (
	"bufio"
	"context"
	"encoding/pem"
	"errors"
	"net"
	"net/http/httptest"
	"sync/atomic"
	"testing"
	"time"
)

func certPEM(srv *httptest.Server) string {
	return string(pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: srv.Certificate().Raw}))
}

// fakeServer answers LIST_NODES like eop-server while `up`, and refuses to answer otherwise.
type fakeServer struct {
	ln  net.Listener
	up  atomic.Bool
	err atomic.Bool // answer ERROR_SERVICE_UNAVAILABLE instead
}

func startFakeServer(t *testing.T) *fakeServer {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	f := &fakeServer{ln: ln}
	f.up.Store(true)
	go func() {
		for {
			conn, err := ln.Accept()
			if err != nil {
				return
			}
			go f.serve(conn)
		}
	}()
	t.Cleanup(func() { _ = ln.Close() })
	return f
}

func (f *fakeServer) serve(conn net.Conn) {
	defer func() { _ = conn.Close() }()
	if !f.up.Load() {
		return // accepted and closed: the client sees EOF
	}
	_, _ = conn.Write([]byte("WELCOME Server v1.0 | TIME 0 | SESSION_ID s\n"))
	line, err := bufio.NewReader(conn).ReadString('\n')
	if err != nil || line != "LIST_NODES\n" {
		return
	}
	if f.err.Load() {
		_, _ = conn.Write([]byte("ERROR_SERVICE_UNAVAILABLE desc=Node registry unavailable\n"))
		return
	}
	_, _ = conn.Write([]byte("LIST_NODES count=1\nnode_id=n1 status=ONLINE\nEND_LIST\n"))
}

func TestAttempt(t *testing.T) {
	f := startFakeServer(t)
	ctx := context.Background()
	if err := Attempt(ctx, f.ln.Addr().String(), time.Second); err != nil {
		t.Fatalf("healthy server: %v", err)
	}
	f.err.Store(true)
	var probeErr *ProbeError
	if err := Attempt(ctx, f.ln.Addr().String(), time.Second); !errors.As(err, &probeErr) || probeErr.Error() == "" {
		t.Fatalf("registry down must count as a failure: %v", err)
	}
	f.err.Store(false)
	f.up.Store(false)
	if err := Attempt(ctx, f.ln.Addr().String(), time.Second); err == nil {
		t.Fatal("closed connection accepted")
	}
	if err := Attempt(ctx, "127.0.0.1:1", time.Second); err == nil {
		t.Fatal("refused connection accepted")
	}
}

func TestAttemptTimesOutOnASilentServer(t *testing.T) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer func() { _ = ln.Close() }()
	go func() {
		conn, err := ln.Accept()
		if err == nil {
			time.Sleep(500 * time.Millisecond)
			_ = conn.Close()
		}
	}()
	start := time.Now()
	if err := Attempt(context.Background(), ln.Addr().String(), 50*time.Millisecond); err == nil {
		t.Fatal("silent server accepted")
	}
	if time.Since(start) > 300*time.Millisecond {
		t.Fatal("the timeout was not applied")
	}
}

// The probe reports one outage covering the time the server was down.
func TestProbeMeasuresAnOutage(t *testing.T) {
	f := startFakeServer(t)
	ctx, cancel := context.WithTimeout(context.Background(), 600*time.Millisecond)
	defer cancel()
	go func() {
		time.Sleep(150 * time.Millisecond)
		f.up.Store(false)
		time.Sleep(200 * time.Millisecond)
		f.up.Store(true)
	}()
	result := Probe(ctx, ProbeConfig{Addr: f.ln.Addr().String(), Interval: 10 * time.Millisecond, Timeout: 100 * time.Millisecond}, time.Now)

	if result.Attempts < 20 || result.Failures == 0 {
		t.Fatalf("%+v", result)
	}
	if len(result.Outages) != 1 {
		t.Fatalf("outages = %+v", result.Outages)
	}
	if result.LongestOutage < 150*time.Millisecond || result.LongestOutage > 300*time.Millisecond {
		t.Fatalf("longest outage %v, expected about 200 ms", result.LongestOutage)
	}
	if result.LongestMs != float64(result.LongestOutage.Microseconds())/1000 {
		t.Fatalf("ms field %v", result.LongestMs)
	}
}

// An outage still open when the probe ends is reported up to the end.
func TestProbeEndingDuringAnOutage(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 100*time.Millisecond)
	defer cancel()
	result := Probe(ctx, ProbeConfig{Addr: "127.0.0.1:1", Interval: 10 * time.Millisecond, Timeout: 20 * time.Millisecond}, time.Now)
	if result.Failures == 0 || result.Failures != result.Attempts || len(result.Outages) != 1 || result.LongestOutage <= 0 {
		t.Fatalf("%+v", result)
	}
}
