package alertack

import (
	"bufio"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

const (
	StoreVersion      = "step46a-v1"
	ackFileName       = "events.jsonl"
	defaultMaxEvents  = 100
	maxSerializedLine = 16 * 1024
	maxStoreRead      = int64(16 << 20)
)

type Event struct {
	Version          string `json:"version"`
	EventID          string `json:"eventId"`
	RecordedAt       string `json:"recordedAt"`
	Actor            string `json:"actor"`
	Action           string `json:"action"`
	AlertID          string `json:"alertId"`
	LifecycleEventID string `json:"lifecycleEventId"`
	Severity         string `json:"severity"`
	Service          string `json:"service"`
	EventType        string `json:"eventType"`
	Title            string `json:"title"`
	Comment          string `json:"comment,omitempty"`
	CorrelationID    string `json:"correlationId"`
	Result           string `json:"result"`
}

type Snapshot struct {
	Available         bool             `json:"available"`
	Error             string           `json:"error,omitempty"`
	Events            []Event          `json:"events"`
	Count             int              `json:"count"`
	LatestByLifecycle map[string]Event `json:"-"`
}

type Store struct {
	mu  sync.Mutex
	dir string
}

func New(dir string) *Store { return &Store{dir: strings.TrimSpace(dir)} }
func (s *Store) Dir() string {
	if s == nil {
		return ""
	}
	return s.dir
}

func (s *Store) Append(event Event) error {
	if s == nil || s.dir == "" {
		return errors.New("alert acknowledgement directory is not configured")
	}
	if strings.TrimSpace(event.EventID) == "" || strings.TrimSpace(event.CorrelationID) == "" {
		return errors.New("alert acknowledgement requires eventId and correlationId")
	}
	if strings.TrimSpace(event.Actor) == "" || strings.TrimSpace(event.AlertID) == "" || strings.TrimSpace(event.LifecycleEventID) == "" {
		return errors.New("alert acknowledgement requires actor, alertId and lifecycleEventId")
	}
	if strings.TrimSpace(event.RecordedAt) == "" {
		event.RecordedAt = time.Now().UTC().Format(time.RFC3339Nano)
	}
	if strings.TrimSpace(event.Version) == "" {
		event.Version = StoreVersion
	}
	if event.Version != StoreVersion {
		return fmt.Errorf("unsupported alert acknowledgement store version %q", event.Version)
	}
	if strings.TrimSpace(event.Action) == "" {
		event.Action = "ALERT_ACKNOWLEDGE"
	}
	if event.Action != "ALERT_ACKNOWLEDGE" {
		return fmt.Errorf("unsupported alert acknowledgement action %q", event.Action)
	}
	if strings.TrimSpace(event.Result) == "" {
		event.Result = "SUCCESS"
	}
	if event.Result != "SUCCESS" {
		return fmt.Errorf("unsupported alert acknowledgement result %q", event.Result)
	}
	event.Comment = strings.TrimSpace(event.Comment)
	if len(event.Comment) > 500 {
		return errors.New("alert acknowledgement comment exceeds 500 characters")
	}

	body, err := json.Marshal(event)
	if err != nil {
		return fmt.Errorf("encode alert acknowledgement: %w", err)
	}
	if len(body) > maxSerializedLine {
		return fmt.Errorf("alert acknowledgement exceeds %d bytes", maxSerializedLine)
	}

	s.mu.Lock()
	defer s.mu.Unlock()
	if err := os.MkdirAll(s.dir, 0o750); err != nil {
		return fmt.Errorf("create alert acknowledgement directory: %w", err)
	}
	path := filepath.Join(s.dir, ackFileName)
	f, err := os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o640)
	if err != nil {
		return fmt.Errorf("open alert acknowledgement store: %w", err)
	}
	defer f.Close()
	if _, err := f.Write(append(body, '\n')); err != nil {
		return fmt.Errorf("append alert acknowledgement: %w", err)
	}
	if err := f.Sync(); err != nil {
		return fmt.Errorf("sync alert acknowledgement: %w", err)
	}
	return nil
}

func ReadSnapshot(dir string, limit int) Snapshot {
	dir = strings.TrimSpace(dir)
	if dir == "" {
		return Snapshot{Available: false, Error: "alert acknowledgement directory is not configured", Events: []Event{}, LatestByLifecycle: map[string]Event{}}
	}
	info, err := os.Stat(dir)
	if err != nil {
		return Snapshot{Available: false, Error: err.Error(), Events: []Event{}, LatestByLifecycle: map[string]Event{}}
	}
	if !info.IsDir() {
		return Snapshot{Available: false, Error: "alert acknowledgement path is not a directory", Events: []Event{}, LatestByLifecycle: map[string]Event{}}
	}
	path := filepath.Join(dir, ackFileName)
	f, err := os.Open(path)
	if errors.Is(err, os.ErrNotExist) {
		return Snapshot{Available: true, Events: []Event{}, Count: 0, LatestByLifecycle: map[string]Event{}}
	}
	if err != nil {
		return Snapshot{Available: false, Error: err.Error(), Events: []Event{}, LatestByLifecycle: map[string]Event{}}
	}
	defer f.Close()
	stat, err := f.Stat()
	if err != nil {
		return Snapshot{Available: false, Error: err.Error(), Events: []Event{}, LatestByLifecycle: map[string]Event{}}
	}
	if stat.Size() > maxStoreRead {
		return Snapshot{Available: false, Error: fmt.Sprintf("alert acknowledgement store exceeds bounded read limit (%d bytes)", maxStoreRead), Events: []Event{}, LatestByLifecycle: map[string]Event{}}
	}
	if limit <= 0 || limit > defaultMaxEvents {
		limit = defaultMaxEvents
	}
	reader := bufio.NewScanner(io.LimitReader(f, maxStoreRead+1))
	reader.Buffer(make([]byte, 4096), maxSerializedLine+1024)
	events := make([]Event, 0, limit)
	latest := make(map[string]Event)
	count := 0
	for reader.Scan() {
		line := strings.TrimSpace(reader.Text())
		if line == "" {
			continue
		}
		var event Event
		if err := json.Unmarshal([]byte(line), &event); err != nil {
			return Snapshot{Available: false, Error: "decode alert acknowledgement: " + err.Error(), Events: []Event{}, LatestByLifecycle: map[string]Event{}}
		}
		if event.Version != StoreVersion || event.Action != "ALERT_ACKNOWLEDGE" || event.Result != "SUCCESS" || strings.TrimSpace(event.LifecycleEventID) == "" {
			return Snapshot{Available: false, Error: "invalid alert acknowledgement event contract", Events: []Event{}, LatestByLifecycle: map[string]Event{}}
		}
		count++
		latest[event.LifecycleEventID] = event
		events = append(events, event)
		if len(events) > limit {
			events = events[len(events)-limit:]
		}
	}
	if err := reader.Err(); err != nil {
		return Snapshot{Available: false, Error: err.Error(), Events: []Event{}, LatestByLifecycle: map[string]Event{}}
	}
	for i, j := 0, len(events)-1; i < j; i, j = i+1, j-1 {
		events[i], events[j] = events[j], events[i]
	}
	return Snapshot{Available: true, Events: events, Count: count, LatestByLifecycle: latest}
}
