package external

import (
	"bytes"
	"context"
	"errors"
	"io/fs"
	"log/slog"
	"os"
	"time"

	"github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0/services/go/internal/breaker"
)

// LoadSettings reads the breaker settings file. An empty path, or a file that does not exist,
// gives the ADR-012 defaults: the gateway must start even without the ConfigMap.
func LoadSettings(path string, services []string) (breaker.Settings, []byte, error) {
	if path == "" {
		settings, err := breaker.ParseSettings(nil, services)
		return settings, nil, err
	}
	data, err := os.ReadFile(path)
	if errors.Is(err, fs.ErrNotExist) {
		settings, perr := breaker.ParseSettings(nil, services)
		return settings, nil, perr
	}
	if err != nil {
		return nil, nil, err
	}
	settings, err := breaker.ParseSettings(data, services)
	return settings, data, err
}

// Watch re-reads path every interval and applies changed settings to the services without
// resetting their circuits (US-303 AC8). Kubernetes updates a mounted ConfigMap in place, so a
// change appears here without a restart or redeploy. An invalid file is logged and ignored: the
// previous settings stay in force.
func Watch(ctx context.Context, path string, interval time.Duration, current []byte, services []*Service, logger *slog.Logger) {
	if path == "" {
		return
	}
	names := make([]string, len(services))
	for i, s := range services {
		names[i] = s.Name()
	}
	ticker := time.NewTicker(interval)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
		}
		data, err := os.ReadFile(path)
		if err != nil || bytes.Equal(data, current) {
			continue
		}
		settings, err := breaker.ParseSettings(data, names)
		if err != nil {
			logger.Error("breaker settings rejected; keeping the previous ones", "path", path, "error", err)
			current = data // do not log the same broken file on every tick
			continue
		}
		for _, s := range services {
			if err := s.Apply(settings.For(s.Name())); err != nil {
				logger.Error("breaker settings not applied", "service", s.Name(), "error", err)
			}
		}
		current = data
		logger.Info("breaker settings reloaded", "path", path)
	}
}
