//go:build integration

package integration

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/alicebob/miniredis/v2"

	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/app"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/config"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/telemetry"
)

// startEngine runs the real hpc_engine binary against the server and waits until it registers.
func startEngine(t *testing.T, serverAddr string) (stop func()) {
	t.Helper()
	bin := os.Getenv("EOP_HPC_ENGINE_BIN")
	if bin == "" {
		t.Fatal("EOP_HPC_ENGINE_BIN must point to the hpc_engine binary")
	}
	host, port, _ := net.SplitHostPort(serverAddr)
	cmd := exec.Command(bin, "--server-host", host, "--server-port", port, "--node-id", "hpc-engine")
	cmd.Env = append(os.Environ(), "EOP_TRACING_ENABLED=0")
	cmd.Stdout, cmd.Stderr = io.Discard, io.Discard
	if err := cmd.Start(); err != nil {
		t.Fatalf("starting hpc_engine: %v", err)
	}
	stopped := false
	stop = func() {
		if !stopped {
			stopped = true
			_ = cmd.Process.Kill()
			_ = cmd.Wait()
		}
	}
	t.Cleanup(stop)

	deadline := time.Now().Add(startupTimeout)
	for time.Now().Before(deadline) {
		if engineListed(serverAddr) {
			return stop
		}
		time.Sleep(100 * time.Millisecond)
	}
	t.Fatal("hpc_engine did not register with eop_server")
	return nil
}

func engineListed(serverAddr string) bool {
	conn, err := net.DialTimeout("tcp", serverAddr, time.Second)
	if err != nil {
		return false
	}
	defer func() { _ = conn.Close() }()
	_ = conn.SetDeadline(time.Now().Add(2 * time.Second))
	reader := bufio.NewReader(conn)
	if _, err := reader.ReadString('\n'); err != nil {
		return false
	}
	if _, err := conn.Write([]byte("LIST_NODES\n")); err != nil {
		return false
	}
	for {
		line, err := reader.ReadString('\n')
		if err != nil || strings.HasPrefix(line, "END_LIST") {
			return false
		}
		if strings.Contains(line, "node_id=hpc-engine ") && strings.Contains(line, "status=ONLINE") {
			return true
		}
	}
}

// runGateway runs the whole gateway (app.Run) against the given server and an in-memory Redis.
func runGateway(t *testing.T, serverAddr string) string {
	t.Helper()
	redisAddr := miniredis.RunT(t).Addr()
	cfg, err := config.Load(func(key string) string {
		return map[string]string{
			"EOP_SERVER_ADDR": serverAddr, "EOP_REDIS_ADDR": redisAddr, "EOP_TRACING_ENABLED": "0",
			"EOP_GATEWAY_SHUTDOWN_TIMEOUT": "5s",
		}[key]
	})
	if err != nil {
		t.Fatal(err)
	}
	apiL, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	internalL, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() {
		done <- app.Run(ctx, cfg, telemetry.NewLogger(io.Discard, "api-gateway"), app.Listeners{API: apiL, Metrics: internalL})
	}()
	t.Cleanup(func() {
		cancel()
		if err := <-done; err != nil {
			t.Errorf("gateway shutdown: %v", err)
		}
	})
	return "http://" + apiL.Addr().String()
}

func chainGraph(nodes int) []byte {
	names := make([]string, nodes)
	edges := make([]map[string]any, 0, nodes)
	for i := range names {
		names[i] = "n" + strconv.Itoa(i)
		if i > 0 {
			edges = append(edges, map[string]any{"from": names[i-1], "to": names[i], "weight": 1})
		}
	}
	body, _ := json.Marshal(map[string]any{"nodes": names, "edges": edges, "algorithms": []string{"connected_components"}})
	return body
}

func post(t *testing.T, url string, body []byte) (int, map[string]any, http.Header) {
	t.Helper()
	resp, err := http.Post(url, "application/json", bytes.NewReader(body))
	if err != nil {
		t.Fatal(err)
	}
	defer func() { _ = resp.Body.Close() }()
	var decoded map[string]any
	if err := json.NewDecoder(resp.Body).Decode(&decoded); err != nil {
		t.Fatal(err)
	}
	return resp.StatusCode, decoded, resp.Header
}

func getMap(t *testing.T, url string) (int, map[string]any) {
	t.Helper()
	resp, err := http.Get(url)
	if err != nil {
		t.Fatal(err)
	}
	defer func() { _ = resp.Body.Close() }()
	var decoded map[string]any
	if err := json.NewDecoder(resp.Body).Decode(&decoded); err != nil {
		t.Fatal(err)
	}
	return resp.StatusCode, decoded
}

func errorCodeOf(body map[string]any) string {
	if e, ok := body["error"].(map[string]any); ok {
		code, _ := e["code"].(string)
		return code
	}
	return ""
}

func TestGraphAnalysisAgainstRealServerAndEngine(t *testing.T) {
	enableTracing(t)
	serverAddr, _ := startServer(t, "EOP_HPC_TIMEOUT_MS=10000")
	stopEngine := startEngine(t, serverAddr)
	gw := runGateway(t, serverAddr)

	t.Run("small graph is answered synchronously (AC1)", func(t *testing.T) {
		status, body, _ := post(t, gw+"/v1/graph/analyze", chainGraph(3))
		result, _ := body["data"].(map[string]any)["result"].(map[string]any)
		if status != http.StatusOK || result["graph_size"].(map[string]any)["nodes"] != 3.0 {
			t.Fatalf("got %d %v", status, body)
		}
	})

	t.Run("invalid graph names the field (AC7)", func(t *testing.T) {
		status, body, _ := post(t, gw+"/v1/graph/analyze", []byte(`{"nodes":["a"],"edges":[{"from":"a","to":"b","weight":1}],"algorithms":["centrality"]}`))
		if status != http.StatusBadRequest || body["error"].(map[string]any)["field"] != "edges[0].to" {
			t.Fatalf("got %d %v", status, body)
		}
	})

	var jobID string
	t.Run("large graph: POST, 202, poll, 200 with results (AC2, AC8)", func(t *testing.T) {
		const nodes = 1200
		status, body, header := post(t, gw+"/v1/graph/analyze", chainGraph(nodes))
		if status != http.StatusAccepted {
			t.Fatalf("got %d %v", status, body)
		}
		jobID = body["data"].(map[string]any)["job_id"].(string)
		if header.Get("Location") != "/v1/graph/jobs/"+jobID {
			t.Fatalf("Location = %q", header.Get("Location"))
		}
		var job map[string]any
		deadline := time.Now().Add(30 * time.Second)
		for time.Now().Before(deadline) {
			code, polled := getMap(t, gw+"/v1/graph/jobs/"+jobID)
			if code != http.StatusOK {
				t.Fatalf("poll = %d %v", code, polled)
			}
			job = polled["data"].(map[string]any)
			if job["status"] == "succeeded" || job["status"] == "failed" {
				break
			}
			time.Sleep(100 * time.Millisecond)
		}
		if job["status"] != "succeeded" {
			t.Fatalf("job = %v", job)
		}
		result := job["result"].(map[string]any)
		components := result["algorithms"].(map[string]any)["connected_components"].([]any)
		if result["graph_size"].(map[string]any)["nodes"] != float64(nodes) || len(components) != 1 {
			t.Fatalf("result = graph_size %v, %d components", result["graph_size"], len(components))
		}
	})

	t.Run("job list shows the job with status and times (AC6)", func(t *testing.T) {
		_, body := getMap(t, gw+"/v1/graph/jobs")
		list := body["data"].([]any)
		found := false
		for _, item := range list {
			j := item.(map[string]any)
			if j["job_id"] == jobID {
				found = j["status"] == "succeeded" && j["created_at"] != nil && j["completed_at"] != nil && j["result"] == nil
			}
		}
		if !found {
			t.Fatalf("job %s missing or incomplete in %v", jobID, list)
		}
	})

	t.Run("engine offline gives 503 without internals (AC3)", func(t *testing.T) {
		stopEngine()
		deadline := time.Now().Add(startupTimeout)
		for engineListed(serverAddr) && time.Now().Before(deadline) {
			time.Sleep(100 * time.Millisecond)
		}
		status, body, _ := post(t, gw+"/v1/graph/analyze", chainGraph(3))
		if status != http.StatusServiceUnavailable || errorCodeOf(body) != "HPC_UNAVAILABLE" {
			t.Fatalf("sync: got %d %v", status, body)
		}
		if strings.Contains(fmt.Sprint(body), "desc=") {
			t.Fatalf("server text leaked: %v", body)
		}
		status, body, _ = post(t, gw+"/v1/graph/analyze", chainGraph(1200))
		if status != http.StatusServiceUnavailable || errorCodeOf(body) != "HPC_UNAVAILABLE" {
			t.Fatalf("async: got %d %v", status, body)
		}
	})
}
