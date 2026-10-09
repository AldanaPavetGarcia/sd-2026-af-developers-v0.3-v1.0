package eopclient

import (
	"bufio"
	"net"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

// fakeServer speaks just enough of the EOP protocol for client tests: it greets every
// connection and answers each request line through handle.
type fakeServer struct {
	t        *testing.T
	listener net.Listener
	banner   string
	handle   func(request string) string

	accepted atomic.Int32
	mu       sync.Mutex
	requests []string
	conns    []net.Conn
	wg       sync.WaitGroup
}

const defaultBanner = "WELCOME Server v1.0 | TIME now | SESSION_ID 1"

func newFakeServer(t *testing.T, handle func(request string) string) *fakeServer {
	t.Helper()
	return newFakeServerWithBanner(t, defaultBanner, handle)
}

// newFakeServerWithBanner greets with banner; an empty banner means the server never greets.
func newFakeServerWithBanner(t *testing.T, banner string, handle func(request string) string) *fakeServer {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	s := &fakeServer{t: t, listener: listener, banner: banner, handle: handle}
	s.wg.Add(1)
	go s.serve()
	t.Cleanup(s.close)
	return s
}

func (s *fakeServer) addr() string { return s.listener.Addr().String() }

func (s *fakeServer) serve() {
	defer s.wg.Done()
	for {
		conn, err := s.listener.Accept()
		if err != nil {
			return
		}
		s.accepted.Add(1)
		s.mu.Lock()
		s.conns = append(s.conns, conn)
		s.mu.Unlock()
		s.wg.Add(1)
		go s.serveConn(conn)
	}
}

func (s *fakeServer) serveConn(conn net.Conn) {
	defer s.wg.Done()
	defer func() { _ = conn.Close() }()
	if s.banner != "" {
		if _, err := conn.Write([]byte(s.banner + "\n")); err != nil {
			return
		}
	}
	reader := bufio.NewReader(conn)
	for {
		line, err := reader.ReadString('\n')
		if err != nil {
			return
		}
		request := strings.TrimRight(line, "\n")
		s.mu.Lock()
		s.requests = append(s.requests, request)
		s.mu.Unlock()
		answer := s.handle(request)
		if answer == "" {
			return // close the connection without answering
		}
		if _, err := conn.Write([]byte(answer)); err != nil {
			return
		}
	}
}

// dropConnections closes every open server-side connection, as the server's idle timeout does.
func (s *fakeServer) dropConnections() {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, conn := range s.conns {
		_ = conn.Close()
	}
	s.conns = nil
}

func (s *fakeServer) recorded() []string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return append([]string(nil), s.requests...)
}

func (s *fakeServer) close() {
	_ = s.listener.Close()
	s.dropConnections()
	s.wg.Wait()
}

func testOptions(addr string) Options {
	return Options{
		Address:        addr,
		DialTimeout:    time.Second,
		RequestTimeout: time.Second,
		IdleTimeout:    time.Minute,
		MaxConns:       2,
		MaxListNodes:   100,
		MaxLineBytes:   4096,
	}
}

func newTestClient(t *testing.T, opts Options) *Client {
	t.Helper()
	client, err := New(opts)
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	t.Cleanup(func() { _ = client.Close() })
	return client
}
