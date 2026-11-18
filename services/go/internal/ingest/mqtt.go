package ingest

import (
	"context"
	"errors"
	"log/slog"
	"sync/atomic"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"

	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/edge"
)

// MQTTConfig is the ingestor's broker session (ADR-028 §2): a fixed client id and a persistent
// session, so the broker keeps QoS 1 messages while the ingestor is away.
type MQTTConfig struct {
	Broker               string // tcp://mosquitto:1883
	ClientID             string
	Username, Password   string
	KeepAlive            time.Duration
	ConnectTimeout       time.Duration
	MaxReconnectInterval time.Duration // reconnection backoff doubles up to this (US-404 AC6)
	Queue                int           // messages received but not yet processed
}

// Subscriber connects to the broker and feeds the Processor in order, acknowledging each message
// only once the Processor is done with it.
type Subscriber struct {
	Config    MQTTConfig
	Processor *Processor
	Logger    *slog.Logger
	state     atomic.Value // edge.State*
}

// State is the broker connection state for the status snapshot.
func (s *Subscriber) State() string {
	if v, ok := s.state.Load().(string); ok {
		return v
	}
	return edge.StateReconnecting
}

func (s *Subscriber) setState(state string) {
	s.state.Store(state)
	connected := 0.0
	if state == edge.StateConnected {
		connected = 1
	}
	s.Processor.Metrics.Connected.Set(connected)
}

// Run serves until ctx ends. Unprocessed messages stay unacknowledged; the broker delivers them
// again on the next session.
func (s *Subscriber) Run(ctx context.Context) error {
	queue := make(chan mqtt.Message, s.Config.Queue)
	filters := map[string]byte{}
	for _, kind := range []string{edge.KindTelemetry, edge.KindEvents, edge.KindHealth} {
		filters[edge.Topic(s.Processor.Prefix, "+", kind)] = 1
	}
	s.setState(edge.StateReconnecting)

	opts := mqtt.NewClientOptions().
		AddBroker(s.Config.Broker).
		SetClientID(s.Config.ClientID).
		SetUsername(s.Config.Username).
		SetPassword(s.Config.Password).
		SetCleanSession(false).
		SetAutoAckDisabled(true).
		SetOrderMatters(true).
		SetKeepAlive(s.Config.KeepAlive).
		SetConnectTimeout(s.Config.ConnectTimeout).
		SetAutoReconnect(true).
		SetConnectRetry(true).
		SetMaxReconnectInterval(s.Config.MaxReconnectInterval).
		SetDefaultPublishHandler(func(_ mqtt.Client, m mqtt.Message) {
			select {
			case queue <- m:
			case <-ctx.Done():
			}
		}).
		SetOnConnectHandler(func(c mqtt.Client) {
			token := c.SubscribeMultiple(filters, nil)
			if token.WaitTimeout(s.Config.ConnectTimeout) && token.Error() == nil {
				s.setState(edge.StateConnected)
				s.Logger.Info("connected to the MQTT broker", "broker", s.Config.Broker, "client_id", s.Config.ClientID)
				return
			}
			s.Logger.Error("subscribing failed; reconnecting", "error", token.Error())
			c.Disconnect(0)
		}).
		SetConnectionLostHandler(func(_ mqtt.Client, err error) {
			s.setState(edge.StateReconnecting)
			s.Logger.Warn("MQTT broker connection lost; reconnecting with backoff", "error", err)
		}).
		SetReconnectingHandler(func(_ mqtt.Client, o *mqtt.ClientOptions) {
			s.setState(edge.StateReconnecting)
			s.Logger.Info("reconnecting to the MQTT broker", "broker", s.Config.Broker)
		})

	client := mqtt.NewClient(opts)
	// With ConnectRetry the token completes only once connected (or on Disconnect).
	client.Connect()
	defer client.Disconnect(uint(s.Config.ConnectTimeout / time.Millisecond))

	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case m := <-queue:
			outcome := s.Processor.Handle(ctx, Message{Topic: m.Topic(), Payload: m.Payload(), ReceivedAt: s.Processor.Now()})
			if outcome == Stopped {
				return ctx.Err()
			}
			m.Ack()
		}
	}
}

// PublishStatus writes the status snapshot every interval until ctx ends (ADR-028 §4).
func PublishStatus(ctx context.Context, interval, ttl, liveness time.Duration, window *edge.Window,
	state func() string, write func(context.Context, edge.Snapshot, time.Duration) error, now func() time.Time, logger *slog.Logger) {
	ticker := time.NewTicker(interval)
	defer ticker.Stop()
	failing := false
	for {
		snap := window.Snapshot(now(), state(), liveness)
		err := write(ctx, snap, ttl)
		switch {
		case err != nil && !failing && !errors.Is(err, context.Canceled):
			logger.Warn("could not publish the edge status snapshot", "error", err)
			failing = true
		case err == nil && failing:
			logger.Info("edge status snapshot published again")
			failing = false
		}
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
		}
	}
}
