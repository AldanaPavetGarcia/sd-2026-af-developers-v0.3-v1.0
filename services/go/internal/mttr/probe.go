package mttr

import (
	"bufio"
	"context"
	"net"
	"strings"
	"time"
)

// ProbeConfig is the synthetic client of `make k8s-mttr-test` (ADR-027 §4): it runs only during
// the test, never as a permanent prober.
type ProbeConfig struct {
	Addr     string        // eop-server Service, host:port
	Interval time.Duration // between attempts (20 ms)
	Timeout  time.Duration // one attempt: connect, LIST_NODES, END_LIST
}

// Outage is a run of consecutive failed attempts: from the start of the first to the start of the
// next successful one.
type Outage struct {
	Start    time.Time     `json:"start"`
	Duration time.Duration `json:"duration_ns"`
}

// ProbeResult summarises a probe run.
type ProbeResult struct {
	Attempts      int           `json:"attempts"`
	Failures      int           `json:"failures"`
	Outages       []Outage      `json:"outages"`
	LongestOutage time.Duration `json:"longest_outage_ns"`
	LongestMs     float64       `json:"longest_outage_ms"`
}

// Attempt is one request through the Service: connect, LIST_NODES, read up to END_LIST. It is a
// full command, so a replica that accepts but cannot reach the registry counts as down.
func Attempt(ctx context.Context, addr string, timeout time.Duration) error {
	ctx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()
	var dialer net.Dialer
	conn, err := dialer.DialContext(ctx, "tcp", addr)
	if err != nil {
		return err
	}
	defer func() { _ = conn.Close() }()
	deadline, _ := ctx.Deadline()
	if err := conn.SetDeadline(deadline); err != nil {
		return err
	}
	if _, err := conn.Write([]byte("LIST_NODES\n")); err != nil {
		return err
	}
	reader := bufio.NewReader(conn)
	for {
		line, err := reader.ReadString('\n')
		if err != nil {
			return err
		}
		line = strings.TrimSpace(line)
		if line == "END_LIST" {
			return nil
		}
		if strings.HasPrefix(line, "ERROR_") {
			return &ProbeError{Line: line}
		}
	}
}

// ProbeError is an error answer from the server.
type ProbeError struct{ Line string }

func (e *ProbeError) Error() string { return "server answered " + e.Line }

// Probe attempts every Interval until ctx ends and reports the outages it saw.
func Probe(ctx context.Context, cfg ProbeConfig, now func() time.Time) ProbeResult {
	var result ProbeResult
	var outageStart time.Time
	inOutage := false
	ticker := time.NewTicker(cfg.Interval)
	defer ticker.Stop()
	end, hasEnd := ctx.Deadline()

	for {
		started := now()
		err := Attempt(ctx, cfg.Addr, cfg.Timeout)
		// An attempt cut by the end of the run is not an outage; ctx.Err() can lag the deadline.
		if ctx.Err() != nil || (hasEnd && !now().Before(end)) {
			break
		}
		result.Attempts++
		switch {
		case err != nil:
			result.Failures++
			if !inOutage {
				inOutage, outageStart = true, started
			}
		case inOutage:
			result.addOutage(outageStart, started.Sub(outageStart))
			inOutage = false
		}
		select {
		case <-ctx.Done():
		case <-ticker.C:
			continue
		}
		break
	}
	if inOutage {
		result.addOutage(outageStart, now().Sub(outageStart))
	}
	return result
}

func (r *ProbeResult) addOutage(start time.Time, d time.Duration) {
	r.Outages = append(r.Outages, Outage{Start: start, Duration: d})
	if d > r.LongestOutage {
		r.LongestOutage = d
		r.LongestMs = float64(d.Microseconds()) / 1000
	}
}
