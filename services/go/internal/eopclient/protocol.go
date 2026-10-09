// Package eopclient is a TCP client for the EOP v0.1 socket protocol (ADR-003, ADR-010).
//
// The protocol is newline-delimited. Text commands answer with space-separated key=value
// lines; errors start with an ERROR_* code and end with a free-text desc= field.
package eopclient

import (
	"errors"
	"fmt"
	"strconv"
	"strings"
	"time"
)

const (
	bannerPrefix     = "WELCOME"
	listHeaderPrefix = "LIST_NODES"
	listTerminator   = "END_LIST"
	heartbeatAck     = "ACK HEARTBEAT"
	errorPrefix      = "ERROR_"
	descKey          = "desc="

	// CodeNotFound is the server's answer to QUERY_NODE for an unknown node_id.
	CodeNotFound = "ERROR_NOT_FOUND"

	// MaxNodeIDLength matches the %63s conversion the server uses to read node_id.
	MaxNodeIDLength = 63
)

// Node is one entry of the server's node registry.
type Node struct {
	NodeID     string
	BunkerName string
	IPAddress  string
	Capacity   int
	Status     string
	LastSeen   time.Time
}

// ErrNotFound is returned when the server does not know the requested node.
var ErrNotFound = errors.New("eopclient: node not found")

// ErrInvalidArgument is returned before anything is sent when an argument could not be
// encoded safely in a protocol line.
var ErrInvalidArgument = errors.New("eopclient: invalid argument")

// ProtocolError is an ERROR_* answer from the server.
type ProtocolError struct {
	Code string
	Desc string
}

func (e *ProtocolError) Error() string {
	return fmt.Sprintf("eopclient: server answered %s: %s", e.Code, e.Desc)
}

// Is lets errors.Is(err, ErrNotFound) match the server's not-found answer.
func (e *ProtocolError) Is(target error) bool {
	return target == ErrNotFound && e.Code == CodeNotFound
}

// MalformedResponseError means the server answered something this client cannot parse.
type MalformedResponseError struct {
	Line   string
	Reason string
}

func (e *MalformedResponseError) Error() string {
	return fmt.Sprintf("eopclient: malformed response %q: %s", e.Line, e.Reason)
}

// ValidNodeID reports whether id can be sent as a node_id without breaking the line framing:
// 1..63 characters from [A-Za-z0-9._-].
func ValidNodeID(id string) bool {
	if id == "" || len(id) > MaxNodeIDLength {
		return false
	}
	for _, r := range id {
		isAlnum := (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') || (r >= '0' && r <= '9')
		if !isAlnum && r != '.' && r != '_' && r != '-' {
			return false
		}
	}
	return true
}

// parseErrorLine turns "ERROR_X key=v desc=free text" into a ProtocolError.
// It returns nil when the line is not an error answer.
func parseErrorLine(line string) *ProtocolError {
	if !strings.HasPrefix(line, errorPrefix) {
		return nil
	}
	code, rest, _ := strings.Cut(line, " ")
	desc := ""
	if idx := strings.Index(rest, descKey); idx >= 0 {
		desc = rest[idx+len(descKey):]
	}
	return &ProtocolError{Code: code, Desc: desc}
}

// parseKeyValues splits "k1=v1 k2=v2" into a map. Values never contain spaces in node records.
func parseKeyValues(line string) (map[string]string, error) {
	fields := strings.Fields(line)
	values := make(map[string]string, len(fields))
	for _, field := range fields {
		key, value, ok := strings.Cut(field, "=")
		if !ok || key == "" {
			return nil, &MalformedResponseError{Line: line, Reason: "expected key=value, got " + strconv.Quote(field)}
		}
		values[key] = value
	}
	return values, nil
}

// parseNode reads a serialized node record:
// "node_id=… bunker_name=… ip_address=… capacity=… status=… last_seen=<epoch seconds>".
func parseNode(line string) (Node, error) {
	values, err := parseKeyValues(line)
	if err != nil {
		return Node{}, err
	}
	for _, key := range []string{"node_id", "bunker_name", "ip_address", "capacity", "status", "last_seen"} {
		if _, ok := values[key]; !ok {
			return Node{}, &MalformedResponseError{Line: line, Reason: "missing " + key}
		}
	}
	capacity, err := strconv.Atoi(values["capacity"])
	if err != nil {
		return Node{}, &MalformedResponseError{Line: line, Reason: "capacity is not an integer"}
	}
	lastSeen, err := strconv.ParseInt(values["last_seen"], 10, 64)
	if err != nil {
		return Node{}, &MalformedResponseError{Line: line, Reason: "last_seen is not epoch seconds"}
	}
	return Node{
		NodeID:     values["node_id"],
		BunkerName: values["bunker_name"],
		IPAddress:  values["ip_address"],
		Capacity:   capacity,
		Status:     values["status"],
		LastSeen:   time.Unix(lastSeen, 0).UTC(),
	}, nil
}

// parseListHeader reads "LIST_NODES count=N" and returns N.
func parseListHeader(line string, maxNodes int) (int, error) {
	command, rest, _ := strings.Cut(line, " ")
	if command != listHeaderPrefix {
		return 0, &MalformedResponseError{Line: line, Reason: "expected LIST_NODES header"}
	}
	values, err := parseKeyValues(rest)
	if err != nil {
		return 0, err
	}
	count, err := strconv.Atoi(values["count"])
	if err != nil || count < 0 {
		return 0, &MalformedResponseError{Line: line, Reason: "count is not a non-negative integer"}
	}
	if count > maxNodes {
		return 0, &MalformedResponseError{Line: line, Reason: fmt.Sprintf("count %d exceeds the limit of %d", count, maxNodes)}
	}
	return count, nil
}
