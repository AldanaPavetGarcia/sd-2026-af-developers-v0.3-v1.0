// Package sim plays edge devices (US-404 AC8, US-405): each one publishes the firmware's exact
// payloads (docs/edge-telemetry-protocol-v1.0.md §2) to the MQTT broker, once per period.
package sim

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"log/slog"
	"math/big"
	"strconv"
	"sync"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"

	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/edge"
)

// Thresholds and walk of the firmware's simulated sensor (Kconfig defaults), in tenths of °C.
const (
	initialTenths = 210
	minTenths     = 150
	maxTenths     = 400
	stepTenths    = 4
	warnTenths    = 300
	alertTenths   = 350
	healthEvery   = 10 // samples between health reports, as CONFIG_EOP_HEALTH_PERIOD_MS / period
	bootIDBytes   = 4
	tenthsPerUnit = 10
)

// Tenths is a temperature written with exactly one decimal, as the firmware prints it (21.0, not 21).
type Tenths int

// MarshalJSON writes the value with one decimal.
func (t Tenths) MarshalJSON() ([]byte, error) {
	sign := ""
	v := int(t)
	if v < 0 {
		sign, v = "-", -v
	}
	return []byte(sign + strconv.Itoa(v/tenthsPerUnit) + "." + strconv.Itoa(v%tenthsPerUnit)), nil
}

// telemetry has the firmware's field order.
type telemetry struct {
	NodeID      string `json:"node_id"`
	BunkerName  string `json:"bunker_name"`
	Timestamp   string `json:"timestamp"`
	Temperature Tenths `json:"temperature"`
	Status      string `json:"status"`
	MessageID   uint32 `json:"message_id"`
	BootID      string `json:"boot_id"`
}

// Device is one simulated bunker.
type Device struct {
	NodeID     string
	BunkerName string
	BootID     string
	messageID  uint32
	tenths     int
}

// NewDevice returns a device with a random boot id, as after a power-on.
func NewDevice(nodeID, bunkerName string) (*Device, error) {
	raw := make([]byte, bootIDBytes)
	if _, err := rand.Read(raw); err != nil {
		return nil, err
	}
	return &Device{NodeID: nodeID, BunkerName: bunkerName, BootID: hex.EncodeToString(raw), tenths: initialTenths}, nil
}

// Next builds the next telemetry payload: message_id +1 and a bounded random walk.
func (d *Device) Next(now time.Time) ([]byte, error) {
	step, err := rand.Int(rand.Reader, big.NewInt(2*stepTenths+1))
	if err != nil {
		return nil, err
	}
	d.tenths = min(max(d.tenths+int(step.Int64())-stepTenths, minTenths), maxTenths)
	d.messageID++
	return json.Marshal(telemetry{
		NodeID: d.NodeID, BunkerName: d.BunkerName, Timestamp: edge.FormatTimestamp(now),
		Temperature: Tenths(d.tenths), Status: status(d.tenths), MessageID: d.messageID, BootID: d.BootID,
	})
}

// Boot is the BOOT event of the first connection.
func (d *Device) Boot(now time.Time) ([]byte, error) {
	return json.Marshal(edge.Event{NodeID: d.NodeID, BootID: d.BootID, Timestamp: edge.FormatTimestamp(now), Event: "BOOT"})
}

// Health is a health report of a device that never restarted a thread.
func (d *Device) Health(now time.Time, uptime time.Duration) ([]byte, error) {
	return json.Marshal(edge.Health{NodeID: d.NodeID, BootID: d.BootID, Timestamp: edge.FormatTimestamp(now),
		UptimeS: uint32(uptime / time.Second), Restarts: map[string]uint32{"sensor": 0, "mqtt": 0}})
}

func status(tenths int) string {
	switch {
	case tenths >= alertTenths:
		return "ALERT"
	case tenths >= warnTenths:
		return "WARN"
	}
	return "OK"
}

// Config is a simulation run.
type Config struct {
	Broker      string
	TopicPrefix string
	NodePrefix  string // node ids are <NodePrefix><n>, n from 1
	Devices     int
	Period      time.Duration // between samples of one device
	Count       int           // samples per device; 0 = until ctx ends
	Timeout     time.Duration // connect and publish acknowledgement
}

// Publisher sends one payload with QoS 1 and waits for the broker's acknowledgement.
type Publisher interface {
	Publish(topic string, payload []byte) error
	Close()
}

// Dial connects a device's MQTT session; the firmware's client id is its node_id.
type Dial func(clientID string) (Publisher, error)

// MQTTDial connects with paho, a persistent session like the firmware's.
func MQTTDial(cfg Config) Dial {
	return func(clientID string) (Publisher, error) {
		opts := mqtt.NewClientOptions().AddBroker(cfg.Broker).SetClientID(clientID).SetCleanSession(false).
			SetConnectTimeout(cfg.Timeout).SetAutoReconnect(true)
		client := mqtt.NewClient(opts)
		token := client.Connect()
		if !token.WaitTimeout(cfg.Timeout) {
			return nil, fmt.Errorf("sim: %s: connect timed out", clientID)
		}
		if token.Error() != nil {
			return nil, fmt.Errorf("sim: %s: %w", clientID, token.Error())
		}
		return &mqttPublisher{client: client, timeout: cfg.Timeout}, nil
	}
}

type mqttPublisher struct {
	client  mqtt.Client
	timeout time.Duration
}

func (p *mqttPublisher) Publish(topic string, payload []byte) error {
	token := p.client.Publish(topic, 1, false, payload)
	if !token.WaitTimeout(p.timeout) {
		return fmt.Errorf("sim: publish to %s timed out", topic)
	}
	return token.Error()
}

func (p *mqttPublisher) Close() { p.client.Disconnect(uint(p.timeout / time.Millisecond)) }

// Run simulates cfg.Devices devices until each published cfg.Count samples (or ctx ends).
// It returns the number of telemetry messages acknowledged by the broker.
func Run(ctx context.Context, cfg Config, dial Dial, now func() time.Time, logger *slog.Logger) (int, error) {
	var (
		wg    sync.WaitGroup
		mu    sync.Mutex
		total int
		first error
	)
	for n := 1; n <= cfg.Devices; n++ {
		nodeID := fmt.Sprintf("%s%d", cfg.NodePrefix, n)
		device, err := NewDevice(nodeID, fmt.Sprintf("Sim-%d", n))
		if err != nil {
			return 0, err
		}
		wg.Add(1)
		go func() {
			defer wg.Done()
			sent, err := runDevice(ctx, cfg, dial, device, now)
			mu.Lock()
			defer mu.Unlock()
			total += sent
			if err != nil && first == nil {
				first = err
			}
			logger.Info("simulated device finished", "node_id", device.NodeID, "boot_id", device.BootID, "sent", sent, "error", err)
		}()
	}
	wg.Wait()
	return total, first
}

func runDevice(ctx context.Context, cfg Config, dial Dial, d *Device, now func() time.Time) (int, error) {
	pub, err := dial(d.NodeID)
	if err != nil {
		return 0, err
	}
	defer pub.Close()
	started := now()
	if err := publish(pub, edge.Topic(cfg.TopicPrefix, d.NodeID, edge.KindEvents), func() ([]byte, error) { return d.Boot(started) }); err != nil {
		return 0, err
	}
	ticker := time.NewTicker(cfg.Period)
	defer ticker.Stop()
	sent := 0
	for cfg.Count == 0 || sent < cfg.Count {
		at := now()
		if err := publish(pub, edge.Topic(cfg.TopicPrefix, d.NodeID, edge.KindTelemetry), func() ([]byte, error) { return d.Next(at) }); err != nil {
			return sent, err
		}
		sent++
		if sent%healthEvery == 0 {
			if err := publish(pub, edge.Topic(cfg.TopicPrefix, d.NodeID, edge.KindHealth), func() ([]byte, error) { return d.Health(at, at.Sub(started)) }); err != nil {
				return sent, err
			}
		}
		if cfg.Count != 0 && sent == cfg.Count {
			break
		}
		select {
		case <-ctx.Done():
			return sent, nil
		case <-ticker.C:
		}
	}
	return sent, nil
}

func publish(pub Publisher, topic string, encode func() ([]byte, error)) error {
	payload, err := encode()
	if err != nil {
		return err
	}
	return pub.Publish(topic, payload)
}
