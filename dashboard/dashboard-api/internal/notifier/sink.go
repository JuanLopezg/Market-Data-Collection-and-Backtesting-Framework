package notifier

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"sync"
)

const (
	NotificationVersion = "step46b-notification-v1"
	maxSinkLine         = 64 * 1024
	maxSinkRead         = int64(32 << 20)
)

type Notification struct {
	Version          string `json:"version"`
	NotificationID   string `json:"notificationId"`
	DeliveredAt      string `json:"deliveredAt"`
	SourceEventID    string `json:"sourceEventId"`
	SourceRecordedAt string `json:"sourceRecordedAt"`
	Transition       string `json:"transition"`
	AlertID          string `json:"alertId"`
	Severity         string `json:"severity"`
	Service          string `json:"service"`
	EventType        string `json:"eventType"`
	Title            string `json:"title"`
	Detail           string `json:"detail,omitempty"`
	CorrelationID    string `json:"correlationId,omitempty"`
	LinkedContext    string `json:"linkedContext,omitempty"`
	Reason           string `json:"reason"`
}

type DeliveryResult struct {
	AlreadyDelivered bool
}

type Sink interface {
	Name() string
	Deliver(context.Context, Notification) (DeliveryResult, error)
}

type TestFileSink struct {
	mu   sync.Mutex
	path string
}

func NewTestFileSink(path string) (*TestFileSink, error) {
	path = strings.TrimSpace(path)
	if path == "" {
		return nil, errors.New("test-file notifier sink path is empty")
	}
	return &TestFileSink{path: path}, nil
}

func (s *TestFileSink) Name() string { return "TEST_FILE" }

func (s *TestFileSink) Deliver(ctx context.Context, n Notification) (DeliveryResult, error) {
	select {
	case <-ctx.Done():
		return DeliveryResult{}, ctx.Err()
	default:
	}
	if strings.TrimSpace(n.NotificationID) == "" || strings.TrimSpace(n.SourceEventID) == "" || strings.TrimSpace(n.AlertID) == "" {
		return DeliveryResult{}, errors.New("notification requires notificationId, sourceEventId and alertId")
	}
	if n.Version == "" {
		n.Version = NotificationVersion
	}
	if n.Version != NotificationVersion {
		return DeliveryResult{}, fmt.Errorf("unsupported notification version %q", n.Version)
	}

	s.mu.Lock()
	defer s.mu.Unlock()
	if err := os.MkdirAll(filepath.Dir(s.path), 0o750); err != nil {
		return DeliveryResult{}, fmt.Errorf("create notifier sink directory: %w", err)
	}
	exists, err := sinkContainsNotification(s.path, n.NotificationID)
	if err != nil {
		return DeliveryResult{}, err
	}
	if exists {
		return DeliveryResult{AlreadyDelivered: true}, nil
	}
	body, err := json.Marshal(n)
	if err != nil {
		return DeliveryResult{}, fmt.Errorf("encode notification: %w", err)
	}
	if len(body) > maxSinkLine {
		return DeliveryResult{}, fmt.Errorf("notification exceeds %d bytes", maxSinkLine)
	}
	f, err := os.OpenFile(s.path, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o640)
	if err != nil {
		return DeliveryResult{}, fmt.Errorf("open test-file notifier sink: %w", err)
	}
	defer f.Close()
	if _, err := f.Write(append(body, '\n')); err != nil {
		return DeliveryResult{}, fmt.Errorf("append test-file notification: %w", err)
	}
	if err := f.Sync(); err != nil {
		return DeliveryResult{}, fmt.Errorf("sync test-file notification: %w", err)
	}
	return DeliveryResult{}, nil
}

func sinkContainsNotification(path, notificationID string) (bool, error) {
	f, err := os.Open(path)
	if errors.Is(err, os.ErrNotExist) {
		return false, nil
	}
	if err != nil {
		return false, fmt.Errorf("open test-file notifier sink for replay: %w", err)
	}
	defer f.Close()
	stat, err := f.Stat()
	if err != nil {
		return false, fmt.Errorf("stat test-file notifier sink: %w", err)
	}
	if stat.Size() > maxSinkRead {
		return false, fmt.Errorf("test-file notifier sink exceeds bounded read limit (%d bytes)", maxSinkRead)
	}
	scanner := bufio.NewScanner(io.LimitReader(f, maxSinkRead+1))
	scanner.Buffer(make([]byte, 4096), maxSinkLine+1024)
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line == "" {
			continue
		}
		var existing Notification
		if err := json.Unmarshal([]byte(line), &existing); err != nil {
			return false, fmt.Errorf("decode test-file notifier sink: %w", err)
		}
		if existing.Version != NotificationVersion || strings.TrimSpace(existing.NotificationID) == "" {
			return false, errors.New("invalid test-file notifier sink event contract")
		}
		if existing.NotificationID == notificationID {
			return true, nil
		}
	}
	if err := scanner.Err(); err != nil {
		return false, fmt.Errorf("read test-file notifier sink: %w", err)
	}
	return false, nil
}
