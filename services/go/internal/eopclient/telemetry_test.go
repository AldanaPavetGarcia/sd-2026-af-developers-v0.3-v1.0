package eopclient

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"

	sdktrace "go.opentelemetry.io/otel/sdk/trace"
)

func sample() Sample {
	return Sample{
		NodeID: "vault-101", BunkerName: "Vault-101", MessageID: 4182, BootID: "9f3c27a1",
		Temperature: 21.4, Status: "OK", SampledAt: time.UnixMilli(1793628187120),
	}
}

const sampleLine = "TELEMETRY node_id=vault-101 bunker_name=Vault-101 message_id=4182 boot_id=9f3c27a1 temperature=21.4 status=OK sampled_at=1793628187120"

func TestTelemetrySendsTheProtocolLine(t *testing.T) {
	server := newFakeServer(t, func(request string) string {
		return "ACK TELEMETRY node_id=vault-101 message_id=4182\n"
	})
	client := newTestClient(t, testOptions(server.addr()))

	if err := client.Telemetry(context.Background(), sample()); err != nil {
		t.Fatalf("Telemetry: %v", err)
	}
	requests := server.recorded()
	if len(requests) != 1 || requests[0] != sampleLine {
		t.Fatalf("requests = %q, want %q", requests, sampleLine)
	}
}

func TestTelemetryCarriesTraceparentLast(t *testing.T) {
	server := newFakeServer(t, func(string) string { return "ACK TELEMETRY node_id=vault-101 message_id=4182\n" })
	client := newTestClient(t, testOptions(server.addr()))
	tp := sdktrace.NewTracerProvider()
	ctx, span := tp.Tracer("test").Start(context.Background(), "edge.ingest")
	defer span.End()

	if err := client.Telemetry(ctx, sample()); err != nil {
		t.Fatalf("Telemetry: %v", err)
	}
	got := server.recorded()[0]
	want := sampleLine + " traceparent=00-" + span.SpanContext().TraceID().String() + "-" + span.SpanContext().SpanID().String() + "-01"
	if got != want {
		t.Fatalf("request = %q\nwant      %q", got, want)
	}
}

func TestTelemetryErrors(t *testing.T) {
	tests := []struct {
		answer    string
		permanent bool
	}{
		{"ERROR_INVALID_REQUEST desc=Reserved node_id\n", true},
		{"ERROR_DUPLICATE desc=node_id registered over TCP\n", true},
		{"ERROR_SERVICE_UNAVAILABLE desc=Node registry unavailable\n", false},
		{"ERROR_BAD_REQUEST desc=Unknown command\n", false},
		{"ACK TELEMETRY node_id=vault-101 message_id=1\n", false}, // wrong message: malformed
	}
	for _, tt := range tests {
		t.Run(strings.Fields(tt.answer)[0], func(t *testing.T) {
			server := newFakeServer(t, func(string) string { return tt.answer })
			client := newTestClient(t, testOptions(server.addr()))
			err := client.Telemetry(context.Background(), sample())
			if err == nil {
				t.Fatal("error answer accepted")
			}
			if IsPermanent(err) != tt.permanent || IsRetryable(err) == tt.permanent {
				t.Fatalf("%v: permanent=%v retryable=%v", err, IsPermanent(err), IsRetryable(err))
			}
		})
	}
}

func TestTelemetryUnreachableServerIsRetryable(t *testing.T) {
	client := newTestClient(t, testOptions("127.0.0.1:1"))
	err := client.Telemetry(context.Background(), sample())
	if !IsRetryable(err) || !IsUnavailable(err) {
		t.Fatalf("err = %v", err)
	}
	if IsRetryable(nil) {
		t.Fatal("nil is not retryable")
	}
}

func TestSampleValidation(t *testing.T) {
	mutations := map[string]func(*Sample){
		"node id":     func(s *Sample) { s.NodeID = "vault 1" },
		"bunker name": func(s *Sample) { s.BunkerName = "" },
		"boot id":     func(s *Sample) { s.BootID = "9F3C27A1" },
		"boot len":    func(s *Sample) { s.BootID = "9f3c" },
		"status":      func(s *Sample) { s.Status = "HOT" },
		"message id":  func(s *Sample) { s.MessageID = 0 },
	}
	for name, mutate := range mutations {
		t.Run(name, func(t *testing.T) {
			s := sample()
			mutate(&s)
			err := s.Validate()
			if !errors.Is(err, ErrInvalidArgument) || !IsPermanent(err) {
				t.Fatalf("Validate = %v", err)
			}
			client := newTestClient(t, testOptions("127.0.0.1:1"))
			if err := client.Telemetry(context.Background(), s); !errors.Is(err, ErrInvalidArgument) {
				t.Fatalf("Telemetry sent an invalid sample: %v", err)
			}
		})
	}
	if err := sample().Validate(); err != nil {
		t.Fatal(err)
	}
}

func TestParseEdgeNode(t *testing.T) {
	node, err := parseNode("node_id=vault-101 bunker_name=Vault-101 ip_address=0.0.0.0 capacity=0 status=ONLINE last_seen=1793628187 source=edge temperature=21.4 device_status=WARN sampled_at=1793628187120")
	if err != nil {
		t.Fatal(err)
	}
	if node.Source != SourceEdge || node.Telemetry == nil || node.Telemetry.Temperature != 21.4 ||
		node.Telemetry.DeviceStatus != "WARN" || !node.Telemetry.SampledAt.Equal(time.UnixMilli(1793628187120)) {
		t.Fatalf("node = %+v telemetry = %+v", node, node.Telemetry)
	}
	tcp, err := parseNode("node_id=a bunker_name=b ip_address=c capacity=1 status=ONLINE last_seen=1")
	if err != nil || tcp.Source != SourceTCP || tcp.Telemetry != nil {
		t.Fatalf("v0.1 record: %+v %v", tcp, err)
	}
	for name, line := range map[string]string{
		"missing key": "node_id=a bunker_name=b ip_address=c capacity=0 status=ONLINE last_seen=1 source=edge temperature=1 device_status=OK",
		"bad temp":    "node_id=a bunker_name=b ip_address=c capacity=0 status=ONLINE last_seen=1 source=edge temperature=x device_status=OK sampled_at=1",
		"bad sampled": "node_id=a bunker_name=b ip_address=c capacity=0 status=ONLINE last_seen=1 source=edge temperature=1 device_status=OK sampled_at=x",
	} {
		var malformed *MalformedResponseError
		if _, err := parseNode(line); !errors.As(err, &malformed) {
			t.Errorf("%s: %v", name, err)
		}
	}
}
