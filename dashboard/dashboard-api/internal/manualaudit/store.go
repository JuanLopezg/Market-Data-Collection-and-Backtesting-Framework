package manualaudit

import (
	"bufio"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"time"
)

const (
	StoreVersion      = "step46-v1"
	defaultMaxRead    = int64(2 << 20)
	defaultMaxEvents  = 100
	manualAuditFile   = "events.jsonl"
	maxSerializedLine = 64 * 1024
)

type Event struct {
	Version                  string   `json:"version"`
	EventID                  string   `json:"eventId"`
	RecordedAt               string   `json:"recordedAt"`
	Actor                    string   `json:"actor"`
	Action                   string   `json:"action"`
	Target                   string   `json:"target"`
	Result                   string   `json:"result"`
	RequestHash              string   `json:"requestHash,omitempty"`
	CorrelationID            string   `json:"correlationId"`
	ReferenceTargetTimestamp string   `json:"referenceTargetTimestamp,omitempty"`
	ContractVersion          string   `json:"contractVersion"`
	Submitted                bool     `json:"submitted"`
	Blockers                 []string `json:"blockers,omitempty"`
	Detail                   string   `json:"detail"`
}

type Snapshot struct {
	Available bool    `json:"available"`
	Error     string  `json:"error,omitempty"`
	Events    []Event `json:"events"`
	Count     int     `json:"count"`
}

type Store struct {
	dir string
}

func New(dir string) *Store {
	return &Store{dir: strings.TrimSpace(dir)}
}

func (s *Store) Dir() string { return s.dir }

func (s *Store) Available() bool {
	if s == nil || s.dir == "" {
		return false
	}
	info, err := os.Stat(s.dir)
	return err == nil && info.IsDir()
}

func (s *Store) Append(event Event) error {
	if s == nil || s.dir == "" {
		return errors.New("manual audit directory is not configured")
	}
	if strings.TrimSpace(event.EventID) == "" || strings.TrimSpace(event.CorrelationID) == "" {
		return errors.New("manual audit event requires eventId and correlationId")
	}
	if strings.TrimSpace(event.Actor) == "" {
		return errors.New("manual audit event requires actor")
	}
	if strings.TrimSpace(event.RecordedAt) == "" {
		event.RecordedAt = time.Now().UTC().Format(time.RFC3339Nano)
	}
	if strings.TrimSpace(event.Version) == "" {
		event.Version = StoreVersion
	}
	if event.ContractVersion == "" {
		event.ContractVersion = StoreVersion
	}
	if event.Result == "" {
		event.Result = "REJECTED"
	}
	if event.Blockers == nil {
		event.Blockers = []string{}
	}
	if err := os.MkdirAll(s.dir, 0o750); err != nil {
		return fmt.Errorf("create manual audit directory: %w", err)
	}
	body, err := json.Marshal(event)
	if err != nil {
		return fmt.Errorf("encode manual audit event: %w", err)
	}
	if len(body) > maxSerializedLine {
		return fmt.Errorf("manual audit event exceeds %d bytes", maxSerializedLine)
	}
	path := filepath.Join(s.dir, manualAuditFile)
	f, err := os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o640)
	if err != nil {
		return fmt.Errorf("open manual audit store: %w", err)
	}
	defer f.Close()
	if _, err := f.Write(append(body, '\n')); err != nil {
		return fmt.Errorf("append manual audit event: %w", err)
	}
	if err := f.Sync(); err != nil {
		return fmt.Errorf("sync manual audit event: %w", err)
	}
	return nil
}

func ReadRecent(dir string, limit int) Snapshot {
	dir = strings.TrimSpace(dir)
	if dir == "" {
		return Snapshot{Available: false, Error: "manual audit directory is not configured", Events: []Event{}}
	}
	info, err := os.Stat(dir)
	if err != nil {
		return Snapshot{Available: false, Error: err.Error(), Events: []Event{}}
	}
	if !info.IsDir() {
		return Snapshot{Available: false, Error: "manual audit path is not a directory", Events: []Event{}}
	}
	path := filepath.Join(dir, manualAuditFile)
	f, err := os.Open(path)
	if errors.Is(err, os.ErrNotExist) {
		return Snapshot{Available: true, Events: []Event{}, Count: 0}
	}
	if err != nil {
		return Snapshot{Available: false, Error: err.Error(), Events: []Event{}}
	}
	defer f.Close()

	stat, err := f.Stat()
	if err != nil {
		return Snapshot{Available: false, Error: err.Error(), Events: []Event{}}
	}
	start := int64(0)
	if stat.Size() > defaultMaxRead {
		start = stat.Size() - defaultMaxRead
		if _, err := f.Seek(start, io.SeekStart); err != nil {
			return Snapshot{Available: false, Error: err.Error(), Events: []Event{}}
		}
		reader := bufio.NewReader(f)
		_, _ = reader.ReadString('\n')
		pos, _ := f.Seek(0, io.SeekCurrent)
		start = pos
	}
	if _, err := f.Seek(start, io.SeekStart); err != nil {
		return Snapshot{Available: false, Error: err.Error(), Events: []Event{}}
	}

	if limit <= 0 || limit > defaultMaxEvents {
		limit = defaultMaxEvents
	}
	reader := bufio.NewScanner(f)
	reader.Buffer(make([]byte, 4096), maxSerializedLine+1024)
	events := make([]Event, 0, limit)
	for reader.Scan() {
		line := strings.TrimSpace(reader.Text())
		if line == "" {
			continue
		}
		var event Event
		if err := json.Unmarshal([]byte(line), &event); err != nil {
			return Snapshot{Available: false, Error: "decode manual audit event: " + err.Error(), Events: []Event{}}
		}
		if event.Version != StoreVersion {
			return Snapshot{Available: false, Error: "unsupported manual audit store version " + event.Version, Events: []Event{}}
		}
		events = append(events, event)
		if len(events) > limit {
			events = events[len(events)-limit:]
		}
	}
	if err := reader.Err(); err != nil {
		return Snapshot{Available: false, Error: err.Error(), Events: []Event{}}
	}
	for i, j := 0, len(events)-1; i < j; i, j = i+1, j-1 {
		events[i], events[j] = events[j], events[i]
	}
	return Snapshot{Available: true, Events: events, Count: len(events)}
}
