package nats

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"time"
)

type MonitorStatus struct {
	Reachable   bool   `json:"reachable"`
	Streams     int    `json:"streams"`
	Consumers   int    `json:"consumers"`
	Messages    uint64 `json:"messages"`
	Bytes       uint64 `json:"bytes"`
	Connections int    `json:"connections"`
	Uptime      string `json:"uptime"`
	Detail      string `json:"detail"`
}

type Monitor struct {
	baseURL string
	client  *http.Client
}

func NewMonitor(baseURL string, timeout time.Duration) *Monitor {
	if timeout <= 0 {
		timeout = 1500 * time.Millisecond
	}
	return &Monitor{
		baseURL: strings.TrimRight(strings.TrimSpace(baseURL), "/"),
		client:  &http.Client{Timeout: timeout},
	}
}

func (m *Monitor) Check(ctx context.Context) (MonitorStatus, error) {
	var result MonitorStatus
	if m.baseURL == "" {
		return result, errors.New("NATS monitoring URL is not configured")
	}
	if parsed, err := url.Parse(m.baseURL); err != nil || parsed.Scheme == "" || parsed.Host == "" {
		return result, errors.New("invalid NATS monitoring URL")
	}

	var jsz struct {
		Streams   int    `json:"streams"`
		Consumers int    `json:"consumers"`
		Messages  uint64 `json:"messages"`
		Bytes     uint64 `json:"bytes"`
	}
	if err := m.getJSON(ctx, "/jsz", &jsz); err != nil {
		return result, err
	}

	var varz struct {
		Connections int    `json:"connections"`
		Uptime      string `json:"uptime"`
	}
	if err := m.getJSON(ctx, "/varz", &varz); err != nil {
		return result, err
	}

	result.Reachable = true
	result.Streams = jsz.Streams
	result.Consumers = jsz.Consumers
	result.Messages = jsz.Messages
	result.Bytes = jsz.Bytes
	result.Connections = varz.Connections
	result.Uptime = varz.Uptime
	result.Detail = "NATS monitoring endpoint reachable"
	return result, nil
}

func (m *Monitor) getJSON(ctx context.Context, path string, target any) error {
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, m.baseURL+path, nil)
	if err != nil {
		return err
	}
	response, err := m.client.Do(request)
	if err != nil {
		return fmt.Errorf("NATS monitor request %s failed: %w", path, err)
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return fmt.Errorf("NATS monitor request %s returned HTTP %d", path, response.StatusCode)
	}
	decoder := json.NewDecoder(io.LimitReader(response.Body, 1<<20))
	if err := decoder.Decode(target); err != nil {
		return fmt.Errorf("decode NATS monitor response %s: %w", path, err)
	}
	return nil
}
