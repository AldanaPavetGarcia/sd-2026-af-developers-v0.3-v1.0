// Package edge holds what the edge ingestor, the device simulator and the API gateway share:
// the device payloads of docs/edge-telemetry-protocol-v1.0.md §2, the MQTT topics, and the status
// snapshot the ingestor publishes in Redis for GET /v1/edge/status (ADR-028 §4).
package edge

import (
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"regexp"
	"strings"
	"time"
)

// Topic layout (protocol §1): <prefix>/<node_id>/<kind>.
const (
	KindTelemetry = "telemetry"
	KindEvents    = "events"
	KindHealth    = "health"

	// TimestampLayout is the device timestamp: RFC 3339 UTC with milliseconds.
	TimestampLayout = "2006-01-02T15:04:05.000Z"

	minTemperature = -50.0
	maxTemperature = 150.0
)

var (
	nodeIDPattern     = regexp.MustCompile(`^[a-z0-9-]{1,63}$`)
	bunkerNamePattern = regexp.MustCompile(`^[A-Za-z0-9_.-]{1,63}$`)
	bootIDPattern     = regexp.MustCompile(`^[0-9a-f]{8}$`)
)

// reservedNodePrefix belongs to the HPC engines (ADR-022).
const reservedNodePrefix = "hpc-engine"

// ErrInvalid wraps every reason a payload is rejected (counted and acknowledged, never forwarded).
var ErrInvalid = errors.New("edge: invalid payload")

// Telemetry is one device sample (protocol §2.1).
type Telemetry struct {
	NodeID      string  `json:"node_id"`
	BunkerName  string  `json:"bunker_name"`
	Timestamp   string  `json:"timestamp"`
	Temperature float64 `json:"temperature"`
	Status      string  `json:"status"`
	MessageID   uint32  `json:"message_id"`
	BootID      string  `json:"boot_id"`
}

// Event is a device event (protocol §2.2): BOOT, or RECONNECTED with Delivered.
type Event struct {
	NodeID    string  `json:"node_id"`
	BootID    string  `json:"boot_id"`
	Timestamp string  `json:"timestamp"`
	Event     string  `json:"event"`
	Delivered *uint32 `json:"delivered,omitempty"`
}

// Health is a device health report (protocol §2.3).
type Health struct {
	NodeID    string            `json:"node_id"`
	BootID    string            `json:"boot_id"`
	Timestamp string            `json:"timestamp"`
	UptimeS   uint32            `json:"uptime_s"`
	Restarts  map[string]uint32 `json:"restarts"`
	Buffered  uint32            `json:"buffered"`
	Dropped   uint32            `json:"dropped"`
}

// Topic builds <prefix>/<node_id>/<kind>.
func Topic(prefix, nodeID, kind string) string {
	return prefix + "/" + nodeID + "/" + kind
}

// SplitTopic returns the node_id and kind of <prefix>/<node_id>/<kind>.
func SplitTopic(prefix, topic string) (nodeID, kind string, err error) {
	rest, ok := strings.CutPrefix(topic, prefix+"/")
	if !ok {
		return "", "", fmt.Errorf("%w: topic %q is not under %q", ErrInvalid, topic, prefix)
	}
	nodeID, kind, ok = strings.Cut(rest, "/")
	if !ok || nodeID == "" || strings.Contains(kind, "/") {
		return "", "", fmt.Errorf("%w: topic %q", ErrInvalid, topic)
	}
	return nodeID, kind, nil
}

// ValidNodeID applies the protocol's node_id rule: [a-z0-9-]{1,63}, not an engine id.
func ValidNodeID(id string) bool {
	return nodeIDPattern.MatchString(id) && !strings.HasPrefix(id, reservedNodePrefix)
}

// ParseTimestamp reads a device timestamp.
func ParseTimestamp(s string) (time.Time, error) {
	t, err := time.Parse(TimestampLayout, s)
	if err != nil {
		return time.Time{}, fmt.Errorf("%w: timestamp %q", ErrInvalid, s)
	}
	return t, nil
}

// FormatTimestamp writes a device timestamp.
func FormatTimestamp(t time.Time) string {
	return t.UTC().Format(TimestampLayout)
}

// decodeStrict rejects unknown fields and trailing data, so a payload is exactly the contract.
func decodeStrict(raw []byte, v any) error {
	dec := json.NewDecoder(strings.NewReader(string(raw)))
	dec.DisallowUnknownFields()
	if err := dec.Decode(v); err != nil {
		return fmt.Errorf("%w: %v", ErrInvalid, err)
	}
	if dec.More() {
		return fmt.Errorf("%w: trailing data", ErrInvalid)
	}
	return nil
}

// ParseTelemetry decodes and validates a telemetry payload published on topic node `topicNode`.
func ParseTelemetry(raw []byte, topicNode string) (Telemetry, time.Time, error) {
	var t Telemetry
	if err := decodeStrict(raw, &t); err != nil {
		return Telemetry{}, time.Time{}, err
	}
	if err := checkIdentity(t.NodeID, t.BootID, topicNode); err != nil {
		return Telemetry{}, time.Time{}, err
	}
	switch {
	case !bunkerNamePattern.MatchString(t.BunkerName):
		return Telemetry{}, time.Time{}, fmt.Errorf("%w: bunker_name %q", ErrInvalid, t.BunkerName)
	case t.Status != "OK" && t.Status != "WARN" && t.Status != "ALERT":
		return Telemetry{}, time.Time{}, fmt.Errorf("%w: status %q", ErrInvalid, t.Status)
	case math.IsNaN(t.Temperature) || t.Temperature < minTemperature || t.Temperature > maxTemperature:
		return Telemetry{}, time.Time{}, fmt.Errorf("%w: temperature %v", ErrInvalid, t.Temperature)
	case t.MessageID == 0:
		return Telemetry{}, time.Time{}, fmt.Errorf("%w: message_id 0", ErrInvalid)
	}
	sampledAt, err := ParseTimestamp(t.Timestamp)
	if err != nil {
		return Telemetry{}, time.Time{}, err
	}
	return t, sampledAt, nil
}

// ParseEvent decodes and validates an event payload.
func ParseEvent(raw []byte, topicNode string) (Event, error) {
	var e Event
	if err := decodeStrict(raw, &e); err != nil {
		return Event{}, err
	}
	if err := checkIdentity(e.NodeID, e.BootID, topicNode); err != nil {
		return Event{}, err
	}
	switch {
	case e.Event == "BOOT" && e.Delivered == nil:
	case e.Event == "RECONNECTED" && e.Delivered != nil:
	default:
		return Event{}, fmt.Errorf("%w: event %q", ErrInvalid, e.Event)
	}
	if _, err := ParseTimestamp(e.Timestamp); err != nil {
		return Event{}, err
	}
	return e, nil
}

// ParseHealth decodes and validates a health payload.
func ParseHealth(raw []byte, topicNode string) (Health, error) {
	var h Health
	if err := decodeStrict(raw, &h); err != nil {
		return Health{}, err
	}
	if err := checkIdentity(h.NodeID, h.BootID, topicNode); err != nil {
		return Health{}, err
	}
	if h.Restarts == nil {
		return Health{}, fmt.Errorf("%w: restarts missing", ErrInvalid)
	}
	if _, err := ParseTimestamp(h.Timestamp); err != nil {
		return Health{}, err
	}
	return h, nil
}

func checkIdentity(nodeID, bootID, topicNode string) error {
	switch {
	case !ValidNodeID(nodeID):
		return fmt.Errorf("%w: node_id %q", ErrInvalid, nodeID)
	case nodeID != topicNode:
		return fmt.Errorf("%w: node_id %q published on the topic of %q", ErrInvalid, nodeID, topicNode)
	case !bootIDPattern.MatchString(bootID):
		return fmt.Errorf("%w: boot_id %q", ErrInvalid, bootID)
	}
	return nil
}
