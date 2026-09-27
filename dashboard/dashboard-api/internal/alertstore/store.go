package alertstore

import (
	"bufio"
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"
)

const (
	eventsFileName   = "events.jsonl"
	statusFileName   = "status.json"
	storeVersion     = "step43-v1"
	maxLifecycleLine = 4 * 1024 * 1024
)

type Alert struct {
	ID            string `json:"id"`
	Timestamp     string `json:"timestamp"`
	Severity      string `json:"severity"`
	Status        string `json:"status"`
	Service       string `json:"service"`
	Asset         string `json:"asset,omitempty"`
	EventType     string `json:"eventType"`
	Title         string `json:"title"`
	Detail        string `json:"detail"`
	CorrelationID string `json:"correlationId,omitempty"`
	LinkedContext string `json:"linkedContext,omitempty"`
}

type LifecycleEvent struct {
	EventID    string `json:"eventId"`
	RecordedAt string `json:"recordedAt"`
	Transition string `json:"transition"`
	Alert      Alert  `json:"alert"`
}

type Heartbeat struct {
	Version        string `json:"version"`
	LastSweepAt    string `json:"lastSweepAt"`
	LastSuccessAt  string `json:"lastSuccessAt"`
	ActiveCount    int    `json:"activeCount"`
	ActiveCritical int    `json:"activeCritical"`
	ActiveWarnings int    `json:"activeWarnings"`
	EventCount     int64  `json:"eventCount"`
	LastError      string `json:"lastError,omitempty"`
}

type ActiveAlert struct {
	LifecycleEventID string `json:"lifecycleEventId"`
	Alert            Alert  `json:"alert"`
}

type Snapshot struct {
	Available bool                   `json:"available"`
	Heartbeat Heartbeat              `json:"heartbeat"`
	Events    []LifecycleEvent       `json:"events"`
	Active    map[string]ActiveAlert `json:"-"`
	Error     string                 `json:"error,omitempty"`
}

type Store struct {
	mu         sync.Mutex
	dir        string
	eventsPath string
	statusPath string
	active     map[string]Alert
	eventCount int64
	heartbeat  Heartbeat
}

func Open(dir string) (*Store, error) {
	dir = strings.TrimSpace(dir)
	if dir == "" {
		return nil, errors.New("alert store directory is empty")
	}
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return nil, fmt.Errorf("create alert store directory: %w", err)
	}
	s := &Store{
		dir:        dir,
		eventsPath: filepath.Join(dir, eventsFileName),
		statusPath: filepath.Join(dir, statusFileName),
		active:     make(map[string]Alert),
		heartbeat:  Heartbeat{Version: storeVersion},
	}
	if err := s.replay(); err != nil {
		return nil, err
	}
	if raw, err := os.ReadFile(s.statusPath); err == nil {
		var hb Heartbeat
		if json.Unmarshal(raw, &hb) == nil && hb.Version == storeVersion {
			s.heartbeat = hb
			if hb.EventCount > s.eventCount {
				s.eventCount = hb.EventCount
			}
		}
	} else if !errors.Is(err, os.ErrNotExist) {
		return nil, fmt.Errorf("read alert watchdog heartbeat: %w", err)
	}
	return s, nil
}

func (s *Store) replay() error {
	raw, err := os.ReadFile(s.eventsPath)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return fmt.Errorf("read alert lifecycle store: %w", err)
	}
	lines := bytes.Split(raw, []byte("\n"))
	for _, line := range lines {
		line = bytes.TrimSpace(line)
		if len(line) == 0 {
			continue
		}
		var event LifecycleEvent
		if err := json.Unmarshal(line, &event); err != nil {
			return fmt.Errorf("decode alert lifecycle store: %w", err)
		}
		s.eventCount++
		switch event.Transition {
		case "OPENED", "UPDATED":
			event.Alert.Status = "ACTIVE"
			s.active[event.Alert.ID] = event.Alert
		case "RESOLVED":
			delete(s.active, event.Alert.ID)
		}
	}
	return nil
}

func (s *Store) Apply(now time.Time, alerts []Alert) (Heartbeat, error) {
	s.mu.Lock()
	defer s.mu.Unlock()

	now = now.UTC()
	current := make(map[string]Alert, len(alerts))
	for _, alert := range alerts {
		alert.ID = strings.TrimSpace(alert.ID)
		if alert.ID == "" || strings.ToUpper(strings.TrimSpace(alert.Status)) != "ACTIVE" {
			continue
		}
		alert.Status = "ACTIVE"
		current[alert.ID] = alert
	}

	transitions := make([]LifecycleEvent, 0)
	ids := make([]string, 0, len(current))
	for id := range current {
		ids = append(ids, id)
	}
	sort.Strings(ids)
	for _, id := range ids {
		alert := current[id]
		previous, exists := s.active[id]
		if !exists {
			transitions = append(transitions, newLifecycleEvent(now, "OPENED", alert, s.eventCount+int64(len(transitions))+1))
			continue
		}
		if materialAlertChange(previous, alert) {
			transitions = append(transitions, newLifecycleEvent(now, "UPDATED", alert, s.eventCount+int64(len(transitions))+1))
		}
	}

	resolvedIDs := make([]string, 0)
	for id := range s.active {
		if _, exists := current[id]; !exists {
			resolvedIDs = append(resolvedIDs, id)
		}
	}
	sort.Strings(resolvedIDs)
	for _, id := range resolvedIDs {
		alert := s.active[id]
		alert.Status = "RESOLVED"
		transitions = append(transitions, newLifecycleEvent(now, "RESOLVED", alert, s.eventCount+int64(len(transitions))+1))
	}

	if len(transitions) > 0 {
		if err := s.appendEvents(transitions); err != nil {
			return s.heartbeat, err
		}
		s.eventCount += int64(len(transitions))
	}
	s.active = current

	hb := Heartbeat{Version: storeVersion, LastSweepAt: now.Format(time.RFC3339), LastSuccessAt: now.Format(time.RFC3339), EventCount: s.eventCount}
	for _, alert := range current {
		hb.ActiveCount++
		switch strings.ToUpper(alert.Severity) {
		case "CRITICAL":
			hb.ActiveCritical++
		case "WARN":
			hb.ActiveWarnings++
		}
	}
	if err := s.writeHeartbeat(hb); err != nil {
		return s.heartbeat, err
	}
	s.heartbeat = hb
	return hb, nil
}

func (s *Store) RecordFailure(now time.Time, err error) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	hb := s.heartbeat
	hb.Version = storeVersion
	hb.LastSweepAt = now.UTC().Format(time.RFC3339)
	if err != nil {
		hb.LastError = err.Error()
	}
	hb.EventCount = s.eventCount
	if writeErr := s.writeHeartbeat(hb); writeErr != nil {
		return writeErr
	}
	s.heartbeat = hb
	return nil
}

func newLifecycleEvent(now time.Time, transition string, alert Alert, sequence int64) LifecycleEvent {
	recorded := now.UTC().Format(time.RFC3339Nano)
	return LifecycleEvent{
		EventID:    fmt.Sprintf("%s:%s:%d", strings.ToLower(transition), alert.ID, sequence),
		RecordedAt: recorded,
		Transition: transition,
		Alert:      alert,
	}
}

func materialAlertChange(a, b Alert) bool {
	return a.Severity != b.Severity || a.Service != b.Service || a.Asset != b.Asset || a.EventType != b.EventType || a.Title != b.Title || a.Detail != b.Detail || a.CorrelationID != b.CorrelationID || a.LinkedContext != b.LinkedContext
}

func (s *Store) appendEvents(events []LifecycleEvent) error {
	f, err := os.OpenFile(s.eventsPath, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o600)
	if err != nil {
		return fmt.Errorf("open alert lifecycle store: %w", err)
	}
	defer f.Close()
	w := bufio.NewWriter(f)
	for _, event := range events {
		raw, err := json.Marshal(event)
		if err != nil {
			return fmt.Errorf("encode alert lifecycle event: %w", err)
		}
		if _, err := w.Write(append(raw, '\n')); err != nil {
			return fmt.Errorf("append alert lifecycle event: %w", err)
		}
	}
	if err := w.Flush(); err != nil {
		return fmt.Errorf("flush alert lifecycle store: %w", err)
	}
	if err := f.Sync(); err != nil {
		return fmt.Errorf("sync alert lifecycle store: %w", err)
	}
	return nil
}

func (s *Store) writeHeartbeat(hb Heartbeat) error {
	raw, err := json.Marshal(hb)
	if err != nil {
		return fmt.Errorf("encode alert watchdog heartbeat: %w", err)
	}
	tmp := s.statusPath + ".tmp"
	if err := os.WriteFile(tmp, raw, 0o600); err != nil {
		return fmt.Errorf("write alert watchdog heartbeat: %w", err)
	}
	if err := os.Rename(tmp, s.statusPath); err != nil {
		return fmt.Errorf("publish alert watchdog heartbeat: %w", err)
	}
	return nil
}

func ReadSnapshot(dir string, maxEvents int) Snapshot {
	dir = strings.TrimSpace(dir)
	if dir == "" {
		return Snapshot{Available: false, Error: "alert watchdog store is not configured"}
	}
	if maxEvents <= 0 {
		maxEvents = 250
	}
	statusPath := filepath.Join(dir, statusFileName)
	rawStatus, err := os.ReadFile(statusPath)
	if err != nil {
		return Snapshot{Available: false, Error: fmt.Sprintf("read alert watchdog heartbeat: %v", err)}
	}
	var hb Heartbeat
	if err := json.Unmarshal(rawStatus, &hb); err != nil {
		return Snapshot{Available: false, Error: fmt.Sprintf("decode alert watchdog heartbeat: %v", err)}
	}
	if hb.Version != storeVersion {
		return Snapshot{Available: false, Error: fmt.Sprintf("unsupported alert watchdog store version %q", hb.Version)}
	}

	snapshot := Snapshot{Available: true, Heartbeat: hb, Events: []LifecycleEvent{}, Active: map[string]ActiveAlert{}}
	rawEvents, err := os.ReadFile(filepath.Join(dir, eventsFileName))
	if errors.Is(err, os.ErrNotExist) {
		return snapshot
	}
	if err != nil {
		snapshot.Available = false
		snapshot.Error = fmt.Sprintf("read alert lifecycle events: %v", err)
		return snapshot
	}
	lines := bytes.Split(rawEvents, []byte("\n"))
	events := make([]LifecycleEvent, 0, minInt(maxEvents, len(lines)))
	for _, line := range lines {
		line = bytes.TrimSpace(line)
		if len(line) == 0 {
			continue
		}
		var event LifecycleEvent
		if err := json.Unmarshal(line, &event); err != nil {
			// A concurrent append can expose only the final partial line. Earlier
			// corruption must still fail closed.
			if bytes.Equal(line, bytes.TrimSpace(lines[len(lines)-1])) && rawEvents[len(rawEvents)-1] != '\n' {
				break
			}
			snapshot.Available = false
			snapshot.Error = fmt.Sprintf("decode alert lifecycle event: %v", err)
			return snapshot
		}
		switch event.Transition {
		case "OPENED", "UPDATED":
			snapshot.Active[event.Alert.ID] = ActiveAlert{LifecycleEventID: event.EventID, Alert: event.Alert}
		case "RESOLVED":
			delete(snapshot.Active, event.Alert.ID)
		}
		if len(events) == maxEvents {
			copy(events, events[1:])
			events[len(events)-1] = event
		} else {
			events = append(events, event)
		}
	}
	for i, j := 0, len(events)-1; i < j; i, j = i+1, j-1 {
		events[i], events[j] = events[j], events[i]
	}
	snapshot.Events = events
	return snapshot
}

// ReadHeartbeat validates the durable watchdog status contract without reading
// the lifecycle log. Independent consumers use it to distinguish a configured,
// initialized source store from an accidentally empty/missing volume.
func ReadHeartbeat(dir string) (Heartbeat, error) {
	dir = strings.TrimSpace(dir)
	if dir == "" {
		return Heartbeat{}, errors.New("alert watchdog store is not configured")
	}
	raw, err := os.ReadFile(filepath.Join(dir, statusFileName))
	if err != nil {
		return Heartbeat{}, fmt.Errorf("read alert watchdog heartbeat: %w", err)
	}
	if len(raw) > 64*1024 {
		return Heartbeat{}, errors.New("alert watchdog heartbeat exceeds bounded read limit")
	}
	var hb Heartbeat
	if err := json.Unmarshal(raw, &hb); err != nil {
		return Heartbeat{}, fmt.Errorf("decode alert watchdog heartbeat: %w", err)
	}
	if hb.Version != storeVersion {
		return Heartbeat{}, fmt.Errorf("unsupported alert watchdog store version %q", hb.Version)
	}
	return hb, nil
}

// ReadLifecycleEvents returns the complete durable lifecycle stream in append
// order. It is intended for independent consumers such as the Step 46B
// notifier. The caller supplies a hard byte bound so a corrupt/unbounded store
// fails closed instead of consuming arbitrary memory.
func ReadLifecycleEvents(dir string, maxBytes int64) ([]LifecycleEvent, error) {
	dir = strings.TrimSpace(dir)
	if dir == "" {
		return nil, errors.New("alert watchdog store is not configured")
	}
	if maxBytes <= 0 {
		return nil, errors.New("alert lifecycle maxBytes must be positive")
	}
	path := filepath.Join(dir, eventsFileName)
	f, err := os.Open(path)
	if errors.Is(err, os.ErrNotExist) {
		return []LifecycleEvent{}, nil
	}
	if err != nil {
		return nil, fmt.Errorf("open alert lifecycle events: %w", err)
	}
	defer f.Close()
	stat, err := f.Stat()
	if err != nil {
		return nil, fmt.Errorf("stat alert lifecycle events: %w", err)
	}
	if stat.Size() > maxBytes {
		return nil, fmt.Errorf("alert lifecycle store exceeds bounded read limit (%d bytes)", maxBytes)
	}

	scanner := bufio.NewScanner(io.LimitReader(f, maxBytes+1))
	// Step43 historically allowed alert details larger than bufio.Scanner's
	// default 64 KiB token limit. Keep a separate bounded per-record ceiling so
	// independent consumers can replay those durable events without becoming
	// unbounded. The whole file is still hard-bounded by maxBytes above.
	scanner.Buffer(make([]byte, 64*1024), maxLifecycleLine)
	events := make([]LifecycleEvent, 0)
	for scanner.Scan() {
		line := bytes.TrimSpace(scanner.Bytes())
		if len(line) == 0 {
			continue
		}
		var event LifecycleEvent
		if err := json.Unmarshal(line, &event); err != nil {
			return nil, fmt.Errorf("decode alert lifecycle event: %w", err)
		}
		if strings.TrimSpace(event.EventID) == "" || strings.TrimSpace(event.Alert.ID) == "" {
			return nil, errors.New("invalid alert lifecycle event contract")
		}
		switch event.Transition {
		case "OPENED", "UPDATED", "RESOLVED":
		default:
			return nil, fmt.Errorf("unsupported alert lifecycle transition %q", event.Transition)
		}
		events = append(events, event)
	}
	if err := scanner.Err(); err != nil {
		return nil, fmt.Errorf("read alert lifecycle events: %w", err)
	}
	return events, nil
}

func minInt(a, b int) int {
	if a < b {
		return a
	}
	return b
}
