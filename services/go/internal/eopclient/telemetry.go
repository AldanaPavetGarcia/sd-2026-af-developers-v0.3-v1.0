package eopclient

import (
	"context"
	"errors"
	"fmt"
	"strconv"
	"time"
)

// Error codes of a TELEMETRY answer (protocol §3).
const (
	CodeInvalidRequest = "ERROR_INVALID_REQUEST"
	CodeDuplicate      = "ERROR_DUPLICATE"

	telemetryAckPrefix = "ACK TELEMETRY "
	bootIDLength       = 8
)

// Sample is one device sample forwarded by the edge ingestor (protocol §2.1 → §3).
type Sample struct {
	NodeID      string
	BunkerName  string
	MessageID   uint32
	BootID      string
	Temperature float64
	Status      string // OK, WARN or ALERT
	SampledAt   time.Time
}

// Validate reports why the sample cannot be encoded in a TELEMETRY line, or nil.
func (s Sample) Validate() error {
	switch {
	case !ValidNodeID(s.NodeID):
		return fmt.Errorf("%w: node_id %q", ErrInvalidArgument, s.NodeID)
	case !ValidNodeID(s.BunkerName):
		return fmt.Errorf("%w: bunker_name %q", ErrInvalidArgument, s.BunkerName)
	case !validBootID(s.BootID):
		return fmt.Errorf("%w: boot_id %q", ErrInvalidArgument, s.BootID)
	case s.Status != "OK" && s.Status != "WARN" && s.Status != "ALERT":
		return fmt.Errorf("%w: status %q", ErrInvalidArgument, s.Status)
	case s.MessageID == 0:
		return fmt.Errorf("%w: message_id 0", ErrInvalidArgument)
	}
	return nil
}

func validBootID(id string) bool {
	if len(id) != bootIDLength {
		return false
	}
	for _, r := range id {
		if (r < '0' || r > '9') && (r < 'a' || r > 'f') {
			return false
		}
	}
	return true
}

// telemetryLine is the §3 command: keys in this order, traceparent (added by the caller) last.
func telemetryLine(s Sample) string {
	return "TELEMETRY node_id=" + s.NodeID +
		" bunker_name=" + s.BunkerName +
		" message_id=" + strconv.FormatUint(uint64(s.MessageID), 10) +
		" boot_id=" + s.BootID +
		" temperature=" + strconv.FormatFloat(s.Temperature, 'f', -1, 64) +
		" status=" + s.Status +
		" sampled_at=" + strconv.FormatInt(s.SampledAt.UnixMilli(), 10)
}

// Telemetry forwards one sample and waits for `ACK TELEMETRY node_id=… message_id=…`. The trace
// context in ctx travels as traceparent= (ADR-028 §3). Sending the same sample twice has the same
// effect as once (the server keeps the newest sampled_at), so a stale pooled connection is retried.
// An ERROR_* answer is a *ProtocolError: see IsRetryable and IsPermanent.
func (c *Client) Telemetry(ctx context.Context, s Sample) error {
	if err := s.Validate(); err != nil {
		return err
	}
	want := telemetryAckPrefix + "node_id=" + s.NodeID + " message_id=" + strconv.FormatUint(uint64(s.MessageID), 10)
	return c.do(ctx, telemetryLine(s), func(pc *conn) error {
		line, err := c.readLine(pc)
		if err != nil {
			return err
		}
		if perr := parseErrorLine(line); perr != nil {
			return perr
		}
		if line != want {
			return &MalformedResponseError{Line: line, Reason: "expected " + want}
		}
		return nil
	})
}

// IsPermanent reports whether the server rejected a sample for good (malformed, reserved id, or a
// node_id registered over TCP): retrying cannot succeed.
func IsPermanent(err error) bool {
	var perr *ProtocolError
	if !errors.As(err, &perr) {
		return errors.Is(err, ErrInvalidArgument)
	}
	return perr.Code == CodeInvalidRequest || perr.Code == CodeDuplicate
}

// IsRetryable reports whether a later attempt may succeed: the server was unreachable, timed out,
// answered something unparseable, said its registry backend is unavailable, or answered an error
// it should not have (an older server without TELEMETRY: waiting for an upgrade loses nothing).
func IsRetryable(err error) bool {
	return err != nil && !IsPermanent(err)
}
