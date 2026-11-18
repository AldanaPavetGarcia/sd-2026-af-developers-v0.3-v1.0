package mttr

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/testutil"
)

func podJSON(name string, isReady, deleting bool) string {
	status := "False"
	if isReady {
		status = "True"
	}
	deletion := ""
	if deleting {
		deletion = `,"deletionTimestamp":"2026-11-20T12:00:00Z"`
	}
	return fmt.Sprintf(`{"metadata":{"name":%q%s},"status":{"conditions":[{"type":"PodScheduled","status":"True"},{"type":"Ready","status":%q}]}}`,
		name, deletion, status)
}

func sliceJSON(name string, endpoints map[string]bool) string {
	var parts []string
	for pod, isReady := range endpoints {
		parts = append(parts, fmt.Sprintf(`{"conditions":{"ready":%t},"targetRef":{"kind":"Pod","name":%q}}`, isReady, pod))
	}
	return fmt.Sprintf(`{"metadata":{"name":%q},"endpoints":[%s]}`, name, strings.Join(parts, ","))
}

func TestPodStateReadsReadinessAndDeletion(t *testing.T) {
	p, err := podState(json.RawMessage(podJSON("a", true, true)), false)
	if err != nil || p != (PodState{Name: "a", Ready: true, Deleting: true}) {
		t.Fatalf("%+v %v", p, err)
	}
	if _, err := podState(json.RawMessage(`{`), false); err == nil {
		t.Fatal("bad JSON accepted")
	}
}

func TestSliceRoutable(t *testing.T) {
	raw := `{"metadata":{"name":"s1"},"endpoints":[
	  {"conditions":{"ready":true},"targetRef":{"kind":"Pod","name":"a"}},
	  {"conditions":{"ready":false},"targetRef":{"kind":"Pod","name":"b"}},
	  {"conditions":{},"targetRef":{"kind":"Pod","name":"c"}},
	  {"conditions":{"ready":true},"targetRef":{"kind":"Node","name":"n"}},
	  {"conditions":{"ready":true}}]}`
	name, routable, err := sliceRoutable(json.RawMessage(raw))
	if err != nil || name != "s1" {
		t.Fatalf("%s %v", name, err)
	}
	want := map[string]bool{"a": true, "b": false, "c": true}
	if fmt.Sprint(routable) != fmt.Sprint(want) {
		t.Fatalf("got %v", routable)
	}
	if _, _, err := sliceRoutable(json.RawMessage(`[`)); err == nil {
		t.Fatal("bad JSON accepted")
	}
}

// fakeAPI serves list + watch for pods and endpointslices; tests push watch events.
type fakeAPI struct {
	mu         sync.Mutex
	listPods   []string
	listSlices []string
	podWatch   chan string
	sliceWatch chan string
	failNext   bool
}

func newFakeAPI() *fakeAPI {
	return &fakeAPI{podWatch: make(chan string, 16), sliceWatch: make(chan string, 16)}
}

func (f *fakeAPI) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	if r.Header.Get("Authorization") != "Bearer test-token" {
		http.Error(w, "no token", http.StatusUnauthorized)
		return
	}
	f.mu.Lock()
	fail := f.failNext
	f.failNext = false
	f.mu.Unlock()
	if fail {
		http.Error(w, "boom", http.StatusInternalServerError)
		return
	}
	isPods := strings.HasSuffix(r.URL.Path, "/pods")
	if r.URL.Query().Get("watch") != "true" {
		items := f.listSlices
		if isPods {
			items = f.listPods
		}
		_, _ = fmt.Fprintf(w, `{"metadata":{"resourceVersion":"7"},"items":[%s]}`, strings.Join(items, ","))
		return
	}
	ch := f.sliceWatch
	if isPods {
		ch = f.podWatch
	}
	flusher := w.(http.Flusher)
	_, _ = fmt.Fprintln(w, `{"type":"BOOKMARK","object":{}}`)
	flusher.Flush()
	for {
		select {
		case <-r.Context().Done():
			return
		case line, ok := <-ch:
			if !ok {
				return
			}
			_, _ = fmt.Fprintln(w, line)
			flusher.Flush()
			// The API ends the stream after an ERROR event; a malformed line ends it on the
			// client side. Either way this handler must not take the next test event.
			if !strings.HasPrefix(line, "{") || strings.Contains(line, `"type":"ERROR"`) {
				return
			}
		}
	}
}

func event(kind, object string) string {
	return fmt.Sprintf(`{"type":%q,"object":%s}`, kind, object)
}

// The whole path: API events → tracker → Prometheus metrics.
func TestWatcherMeasuresAFailureFromTheAPI(t *testing.T) {
	api := newFakeAPI()
	api.listPods = []string{podJSON("a", true, false), podJSON("b", true, false)}
	api.listSlices = []string{sliceJSON("eop-server-x", map[string]bool{"a": true, "b": true})}
	srv := httptest.NewServer(api)
	defer srv.Close()

	clock := make(chan time.Time, 16)
	now := func() time.Time { return <-clock }
	reg := prometheus.NewRegistry()
	metrics := NewMetrics(reg)
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	w := &Watcher{
		Kube:        &Kube{BaseURL: srv.URL, Namespace: "default", Token: "test-token", Client: srv.Client(), Logger: logger},
		PodSelector: "app=eop-server", Service: "eop-server", Metrics: metrics, Logger: logger, Now: now,
	}
	// One timestamp per event, in arrival order: 2 listed pods + 1 listed slice, then the watch.
	for i := 0; i < 3; i++ {
		clock <- t0
	}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- w.Run(ctx) }()

	waitFor(t, func() bool { return testutil.ToFloat64(metrics.ready) == 2 })
	clock <- ms(0)
	api.podWatch <- event("MODIFIED", podJSON("a", true, true))
	waitFor(t, func() bool { return testutil.ToFloat64(metrics.failures) == 1 })
	clock <- ms(40)
	api.sliceWatch <- event("MODIFIED", sliceJSON("eop-server-x", map[string]bool{"b": true}))
	waitFor(t, func() bool {
		return testutil.ToFloat64(metrics.last.WithLabelValues(string(ServiceRestoration))) == 0.04
	})
	clock <- ms(2500)
	api.podWatch <- event("ADDED", podJSON("c", true, false))
	waitFor(t, func() bool {
		return testutil.ToFloat64(metrics.last.WithLabelValues(string(ReplicaReplacement))) == 2.5
	})
	clock <- ms(2600)
	api.sliceWatch <- event("DELETED", sliceJSON("eop-server-x", nil))
	waitFor(t, func() bool { return testutil.ToFloat64(metrics.ready) == 0 })
	// Unreadable events are skipped.
	clock <- ms(2700)
	api.podWatch <- event("MODIFIED", `"not a pod"`)
	clock <- ms(2700)
	api.sliceWatch <- event("MODIFIED", `"not a slice"`)
	if n := testutil.CollectAndCount(metrics.durations); n != 2 {
		t.Fatalf("histogram series = %d", n)
	}

	cancel()
	if err := <-done; err != context.Canceled {
		t.Fatalf("Run returned %v", err)
	}
}

// A watch that breaks (or an API error) is followed by a new list, not by giving up.
func TestWatchRelistsAfterABrokenWatch(t *testing.T) {
	old := watchRetryDelay
	watchRetryDelay = time.Millisecond
	defer func() { watchRetryDelay = old }()

	api := newFakeAPI()
	api.listPods = []string{podJSON("a", true, false)}
	api.failNext = true
	srv := httptest.NewServer(api)
	defer srv.Close()
	kube := &Kube{BaseURL: srv.URL, Namespace: "default", Token: "test-token", Client: srv.Client(),
		Logger: slog.New(slog.NewTextHandler(io.Discard, nil))}

	events := make(chan Event, 16)
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- kube.Watch(ctx, "/api/v1/namespaces/default/pods", "app=eop-server", events) }()

	first := <-events // after the failed request, a successful list
	if first.Type != "ADDED" {
		t.Fatalf("got %s", first.Type)
	}
	api.podWatch <- event("ERROR", `{"code":410}`) // expired resourceVersion: list again
	second := <-events
	if second.Type != "ADDED" {
		t.Fatalf("got %s", second.Type)
	}
	api.podWatch <- `not json`
	if third := <-events; third.Type != "ADDED" {
		t.Fatalf("got %s", third.Type)
	}
	cancel()
	if err := <-done; err != context.Canceled {
		t.Fatalf("Watch returned %v", err)
	}
}

func TestRequestErrors(t *testing.T) {
	kube := &Kube{BaseURL: "http://127.0.0.1:1", Token: "x", Client: http.DefaultClient}
	if _, err := kube.list(context.Background(), "/api/v1/pods", "", make(chan Event)); err == nil {
		t.Fatal("unreachable API accepted")
	}
	bad := &Kube{BaseURL: "http://bad host", Client: http.DefaultClient}
	if _, err := bad.request(context.Background(), "/x", nil); err == nil {
		t.Fatal("bad URL accepted")
	}
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { _, _ = fmt.Fprint(w, "{") }))
	defer srv.Close()
	broken := &Kube{BaseURL: srv.URL, Client: srv.Client()}
	if _, err := broken.list(context.Background(), "/x", "", make(chan Event)); err == nil {
		t.Fatal("bad list accepted")
	}
}

func TestInClusterConfiguration(t *testing.T) {
	dir := t.TempDir()
	if _, err := inClusterFrom(dir, "", ""); err == nil {
		t.Fatal("outside a cluster accepted")
	}
	if _, err := inClusterFrom(dir, "10.0.0.1", "443"); err == nil {
		t.Fatal("missing token accepted")
	}
	write := func(name, content string) {
		if err := os.WriteFile(filepath.Join(dir, name), []byte(content), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	write("token", "tok\n")
	if _, err := inClusterFrom(dir, "10.0.0.1", "443"); err == nil {
		t.Fatal("missing namespace accepted")
	}
	write("namespace", "eop\n")
	if _, err := inClusterFrom(dir, "10.0.0.1", "443"); err == nil {
		t.Fatal("missing CA accepted")
	}
	write("ca.crt", "not a certificate")
	if _, err := inClusterFrom(dir, "10.0.0.1", "443"); err == nil {
		t.Fatal("bad CA accepted")
	}
	srv := httptest.NewTLSServer(http.NotFoundHandler())
	defer srv.Close()
	pem := certPEM(srv)
	write("ca.crt", pem)
	kube, err := inClusterFrom(dir, "10.0.0.1", "443")
	if err != nil {
		t.Fatal(err)
	}
	if kube.BaseURL != "https://10.0.0.1:443" || kube.Namespace != "eop" || kube.Token != "tok" {
		t.Fatalf("%+v", kube)
	}
	if _, err := InCluster(); err == nil && os.Getenv("KUBERNETES_SERVICE_HOST") == "" {
		t.Fatal("InCluster outside a cluster")
	}
}

func waitFor(t *testing.T, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if cond() {
			return
		}
		time.Sleep(5 * time.Millisecond)
	}
	t.Fatal("condition not met in 5 s")
}
