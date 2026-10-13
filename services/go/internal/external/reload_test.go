package external

import (
	"bytes"
	"context"
	"io"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/breaker"
	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/telemetry"
)

type syncBuffer struct {
	mu  sync.Mutex
	buf bytes.Buffer
}

func (b *syncBuffer) Write(p []byte) (int, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.buf.Write(p)
}

func (b *syncBuffer) String() string {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.buf.String()
}

func TestLoadSettings(t *testing.T) {
	names := []string{"ai"}
	for _, path := range []string{"", filepath.Join(t.TempDir(), "missing.yaml")} {
		settings, data, err := LoadSettings(path, names)
		if err != nil || data != nil || settings.For("ai") != breaker.Defaults() {
			t.Fatalf("path %q: %+v %v", path, settings, err)
		}
	}
	path := filepath.Join(t.TempDir(), "breaker.yaml")
	_ = os.WriteFile(path, []byte("defaults:\n  failure_threshold: 2\n"), 0o600)
	settings, data, err := LoadSettings(path, names)
	if err != nil || settings.For("ai").Breaker.FailureThreshold != 2 || len(data) == 0 {
		t.Fatalf("%+v %v", settings, err)
	}
	_ = os.WriteFile(path, []byte("defaults:\n  bogus: 1\n"), 0o600)
	if _, _, err := LoadSettings(path, names); err == nil {
		t.Fatal("invalid file accepted at startup")
	}
	dir := t.TempDir() // a directory cannot be read as a file
	if _, _, err := LoadSettings(dir, names); err == nil {
		t.Fatal("unreadable path accepted")
	}
}

func TestWatchAppliesChangesWithoutResettingTheCircuit(t *testing.T) {
	s, _, _ := newService(t, "500")
	long := fastSettings()
	long.Breaker.ResetTimeout = time.Hour // the circuit must not leave OPEN on its own during the test
	if err := s.Apply(long); err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 3; i++ {
		_, _ = s.Get(context.Background(), "/x", "k")
	}
	if s.Breaker().State() != breaker.Open {
		t.Fatal("setup: circuit should be open")
	}

	path := filepath.Join(t.TempDir(), "breaker.yaml")
	initial := []byte("defaults:\n  failure_threshold: 3\n  reset_timeout: 100ms\n")
	_ = os.WriteFile(path, initial, 0o600)
	logs := &syncBuffer{}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() {
		Watch(ctx, path, 10*time.Millisecond, initial, []*Service{s}, telemetry.NewLogger(logs, "api-gateway"))
		close(done)
	}()

	writeAtomically(t, path, "defaults:\n  failure_threshold: 7\n  reset_timeout: 1h\n  call_timeout: 3s\n")
	waitUntil(t, func() bool { return s.Breaker().Config().FailureThreshold == 7 })
	if s.Breaker().State() != breaker.Open {
		t.Fatal("reload reset the circuit (US-303 AC8 requires it to keep its state)")
	}

	writeAtomically(t, path, "defaults:\n  failure_threshold: [\n")
	waitUntil(t, func() bool { return strings.Contains(logs.String(), "breaker settings rejected") })
	if s.Breaker().Config().FailureThreshold != 7 {
		t.Fatal("a broken file must keep the previous settings")
	}
	cancel()
	<-done
	if !strings.Contains(logs.String(), "breaker settings reloaded") {
		t.Fatalf("logs = %s", logs.String())
	}
	Watch(context.Background(), "", time.Millisecond, nil, nil, nil) // no path: returns at once
}

// TestWatchIgnoresAFileCaughtMidWrite reproduces an in-place edit read between truncation and
// write: the empty file is valid (all defaults) but must not be applied.
func TestWatchIgnoresAFileCaughtMidWrite(t *testing.T) {
	s, _, _ := newService(t, "ok")
	custom := fastSettings()
	custom.Breaker.FailureThreshold = 9
	if err := s.Apply(custom); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(t.TempDir(), "breaker.yaml")
	initial := []byte("defaults:\n  failure_threshold: 9\n")
	_ = os.WriteFile(path, initial, 0o600)
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() {
		Watch(ctx, path, 20*time.Millisecond, initial, []*Service{s}, telemetry.NewLogger(io.Discard, "api-gateway"))
		close(done)
	}()

	_ = os.WriteFile(path, nil, 0o600) // truncated and left empty for several intervals
	time.Sleep(100 * time.Millisecond)
	if s.Breaker().Config().FailureThreshold != 9 {
		t.Fatal("an empty file must never be applied on reload")
	}
	writeAtomically(t, path, "defaults:\n  failure_threshold: 4\n")
	waitUntil(t, func() bool { return s.Breaker().Config().FailureThreshold == 4 })
	cancel()
	<-done
}

// writeAtomically replaces path in one step, as kubelet does with ConfigMap volumes.
func writeAtomically(t *testing.T, path, content string) {
	t.Helper()
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, []byte(content), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.Rename(tmp, path); err != nil {
		t.Fatal(err)
	}
}

func waitUntil(t *testing.T, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		if cond() {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatal("condition not met in time")
}
