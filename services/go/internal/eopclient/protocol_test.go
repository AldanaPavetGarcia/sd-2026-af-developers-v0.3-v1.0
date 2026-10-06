package eopclient

import (
	"errors"
	"strings"
	"testing"
	"time"
)

func TestValidNodeID(t *testing.T) {
	tests := []struct {
		id   string
		want bool
	}{
		{"alpha-01", true},
		{"node_1.zone-B", true},
		{strings.Repeat("a", MaxNodeIDLength), true},
		{"", false},
		{strings.Repeat("a", MaxNodeIDLength+1), false},
		{"alpha 01", false},
		{"alpha\nLIST_NODES", false},
		{"alpha=1", false},
		{"ñandú", false},
	}
	for _, tt := range tests {
		if got := ValidNodeID(tt.id); got != tt.want {
			t.Errorf("ValidNodeID(%q) = %v, want %v", tt.id, got, tt.want)
		}
	}
}

func TestParseNode(t *testing.T) {
	node, err := parseNode("node_id=alpha-01 bunker_name=Zone-B ip_address=10.0.0.6 capacity=10 status=ONLINE last_seen=1759700000")
	if err != nil {
		t.Fatalf("parseNode: %v", err)
	}
	want := Node{NodeID: "alpha-01", BunkerName: "Zone-B", IPAddress: "10.0.0.6", Capacity: 10, Status: "ONLINE", LastSeen: time.Unix(1759700000, 0).UTC()}
	if node != want {
		t.Fatalf("parseNode = %+v, want %+v", node, want)
	}
}

func TestParseNodeRejectsMalformedRecords(t *testing.T) {
	tests := map[string]string{
		"missing field":     "node_id=a bunker_name=b ip_address=c capacity=1 status=ONLINE",
		"capacity not int":  "node_id=a bunker_name=b ip_address=c capacity=x status=ONLINE last_seen=1",
		"last_seen not int": "node_id=a bunker_name=b ip_address=c capacity=1 status=ONLINE last_seen=yesterday",
		"field without =":   "node_id=a bunker_name b",
		"field with no key": "=a",
	}
	for name, line := range tests {
		t.Run(name, func(t *testing.T) {
			var malformed *MalformedResponseError
			if _, err := parseNode(line); !errors.As(err, &malformed) {
				t.Fatalf("parseNode(%q) error = %v, want MalformedResponseError", line, err)
			}
		})
	}
}

func TestParseErrorLine(t *testing.T) {
	perr := parseErrorLine("ERROR_NOT_FOUND node_id=ghost desc=Node not found")
	if perr == nil || perr.Code != CodeNotFound || perr.Desc != "Node not found" {
		t.Fatalf("parseErrorLine = %+v", perr)
	}
	if !errors.Is(perr, ErrNotFound) {
		t.Fatal("ERROR_NOT_FOUND must match ErrNotFound")
	}
	other := parseErrorLine("ERROR_BAD_REQUEST")
	if other == nil || other.Code != "ERROR_BAD_REQUEST" || other.Desc != "" {
		t.Fatalf("parseErrorLine without desc = %+v", other)
	}
	if errors.Is(other, ErrNotFound) {
		t.Fatal("ERROR_BAD_REQUEST must not match ErrNotFound")
	}
	if !strings.Contains(other.Error(), "ERROR_BAD_REQUEST") {
		t.Fatalf("Error() = %q", other.Error())
	}
	if parseErrorLine("node_id=a") != nil {
		t.Fatal("a record line is not an error")
	}
}

func TestParseListHeader(t *testing.T) {
	if n, err := parseListHeader("LIST_NODES count=3", 10); err != nil || n != 3 {
		t.Fatalf("parseListHeader = %d, %v", n, err)
	}
	for _, line := range []string{"LIST_NODES count=-1", "LIST_NODES count=x", "LIST_NODES", "NODES count=1", "LIST_NODES count=11", "LIST_NODES =1"} {
		if _, err := parseListHeader(line, 10); err == nil {
			t.Errorf("parseListHeader(%q) accepted a malformed header", line)
		}
	}
}
