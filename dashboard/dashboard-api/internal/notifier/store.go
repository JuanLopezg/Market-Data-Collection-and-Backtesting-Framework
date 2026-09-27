package notifier

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
	StoreVersion         = "step46b-v1"
	decisionsFileName    = "events.jsonl"
	statusFileName       = "status.json"
	maxSerializedLine    = 32 * 1024
	defaultMaxStoreBytes = int64(32 << 20)
)

type Decision struct {
	Version          string `json:"version"`
	DecisionID       string `json:"decisionId"`
	RecordedAt       string `json:"recordedAt"`
	SourceEventID    string `json:"sourceEventId,omitempty"`
	SourceRecordedAt string `json:"sourceRecordedAt,omitempty"`
	AlertID          string `json:"alertId,omitempty"`
	Transition       string `json:"transition,omitempty"`
	Severity         string `json:"severity,omitempty"`
	Service          string `json:"service,omitempty"`
	EventType        string `json:"eventType,omitempty"`
	Title            string `json:"title,omitempty"`
	Action           string `json:"action"`
	Reason           string `json:"reason,omitempty"`
	NotificationID   string `json:"notificationId,omitempty"`
	Sink             string `json:"sink,omitempty"`
}

type Heartbeat struct {
	Version           string `json:"version"`
	LastSweepAt       string `json:"lastSweepAt"`
	LastSuccessAt     string `json:"lastSuccessAt,omitempty"`
	SourceEventCount  int    `json:"sourceEventCount"`
	ProcessedCount    int    `json:"processedCount"`
	DeliveredCount    int    `json:"deliveredCount"`
	SuppressedCount   int    `json:"suppressedCount"`
	LastSourceEventID string `json:"lastSourceEventId,omitempty"`
	BootstrapComplete bool   `json:"bootstrapComplete"`
	Sink              string `json:"sink"`
	MinSeverity       string `json:"minSeverity"`
	Cooldown          string `json:"cooldown"`
	LastError         string `json:"lastError,omitempty"`
}

type DeliveryState struct {
	SourceRecordedAt string
	Severity         string
	NotificationID   string
}

type Store struct {
	mu                sync.Mutex
	dir               string
	decisionsPath     string
	statusPath        string
	processed         map[string]Decision
	lastDelivery      map[string]DeliveryState
	bootstrapComplete bool
	processedCount    int
	deliveredCount    int
	suppressedCount   int
	lastSourceEventID string
	heartbeat         Heartbeat
}

func OpenStore(dir string) (*Store, error) {
	dir = strings.TrimSpace(dir)
	if dir == "" {
		return nil, errors.New("notifier store directory is empty")
	}
	if err := os.MkdirAll(dir, 0o750); err != nil {
		return nil, fmt.Errorf("create notifier store directory: %w", err)
	}
	s := &Store{
		dir:           dir,
		decisionsPath: filepath.Join(dir, decisionsFileName),
		statusPath:    filepath.Join(dir, statusFileName),
		processed:     make(map[string]Decision),
		lastDelivery:  make(map[string]DeliveryState),
		heartbeat:     Heartbeat{Version: StoreVersion},
	}
	if err := s.replay(defaultMaxStoreBytes); err != nil {
		return nil, err
	}
	return s, nil
}

func (s *Store) replay(maxBytes int64) error {
	f, err := os.Open(s.decisionsPath)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return fmt.Errorf("open notifier decision store: %w", err)
	}
	defer f.Close()
	stat, err := f.Stat()
	if err != nil {
		return fmt.Errorf("stat notifier decision store: %w", err)
	}
	if stat.Size() > maxBytes {
		return fmt.Errorf("notifier decision store exceeds bounded read limit (%d bytes)", maxBytes)
	}
	scanner := bufio.NewScanner(io.LimitReader(f, maxBytes+1))
	scanner.Buffer(make([]byte, 4096), maxSerializedLine+1024)
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line == "" {
			continue
		}
		var d Decision
		if err := json.Unmarshal([]byte(line), &d); err != nil {
			return fmt.Errorf("decode notifier decision store: %w", err)
		}
		if d.Version != StoreVersion || strings.TrimSpace(d.DecisionID) == "" {
			return errors.New("invalid notifier decision contract")
		}
		if d.Action == "BOOTSTRAP_COMPLETE" {
			s.bootstrapComplete = true
			continue
		}
		if d.Action != "DELIVERED" && d.Action != "SUPPRESSED" {
			return fmt.Errorf("unsupported notifier action %q", d.Action)
		}
		if strings.TrimSpace(d.SourceEventID) == "" || strings.TrimSpace(d.AlertID) == "" {
			return errors.New("notifier decision missing sourceEventId or alertId")
		}
		if _, exists := s.processed[d.SourceEventID]; exists {
			return fmt.Errorf("duplicate notifier source event decision %q", d.SourceEventID)
		}
		s.processed[d.SourceEventID] = d
		s.processedCount++
		s.lastSourceEventID = d.SourceEventID
		if d.Action == "DELIVERED" {
			s.deliveredCount++
			s.lastDelivery[d.AlertID] = DeliveryState{SourceRecordedAt: d.SourceRecordedAt, Severity: d.Severity, NotificationID: d.NotificationID}
		} else {
			s.suppressedCount++
		}
	}
	if err := scanner.Err(); err != nil {
		return fmt.Errorf("read notifier decision store: %w", err)
	}
	return nil
}

func (s *Store) IsProcessed(sourceEventID string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	_, ok := s.processed[sourceEventID]
	return ok
}

func (s *Store) BootstrapComplete() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.bootstrapComplete
}

func (s *Store) LastDelivery(alertID string) (DeliveryState, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	v, ok := s.lastDelivery[alertID]
	return v, ok
}

func (s *Store) AppendDecision(d Decision) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if strings.TrimSpace(d.SourceEventID) == "" || strings.TrimSpace(d.AlertID) == "" {
		return errors.New("notifier decision requires sourceEventId and alertId")
	}
	if _, exists := s.processed[d.SourceEventID]; exists {
		return nil
	}
	if d.Action != "DELIVERED" && d.Action != "SUPPRESSED" {
		return fmt.Errorf("unsupported notifier action %q", d.Action)
	}
	if strings.TrimSpace(d.Version) == "" {
		d.Version = StoreVersion
	}
	if d.Version != StoreVersion {
		return fmt.Errorf("unsupported notifier decision version %q", d.Version)
	}
	if strings.TrimSpace(d.DecisionID) == "" {
		d.DecisionID = "decision:" + d.SourceEventID
	}
	if strings.TrimSpace(d.RecordedAt) == "" {
		d.RecordedAt = time.Now().UTC().Format(time.RFC3339Nano)
	}
	if d.Action == "DELIVERED" && (strings.TrimSpace(d.NotificationID) == "" || strings.TrimSpace(d.Sink) == "") {
		return errors.New("delivered notifier decision requires notificationId and sink")
	}
	if err := s.append(d); err != nil {
		return err
	}
	s.processed[d.SourceEventID] = d
	s.processedCount++
	s.lastSourceEventID = d.SourceEventID
	if d.Action == "DELIVERED" {
		s.deliveredCount++
		s.lastDelivery[d.AlertID] = DeliveryState{SourceRecordedAt: d.SourceRecordedAt, Severity: d.Severity, NotificationID: d.NotificationID}
	} else {
		s.suppressedCount++
	}
	return nil
}

func (s *Store) MarkBootstrapComplete(now time.Time) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.bootstrapComplete {
		return nil
	}
	d := Decision{
		Version:    StoreVersion,
		DecisionID: "bootstrap-complete",
		RecordedAt: now.UTC().Format(time.RFC3339Nano),
		Action:     "BOOTSTRAP_COMPLETE",
		Reason:     "initial durable lifecycle replay complete",
	}
	if err := s.append(d); err != nil {
		return err
	}
	s.bootstrapComplete = true
	return nil
}

func (s *Store) append(d Decision) error {
	body, err := json.Marshal(d)
	if err != nil {
		return fmt.Errorf("encode notifier decision: %w", err)
	}
	if len(body) > maxSerializedLine {
		return fmt.Errorf("notifier decision exceeds %d bytes", maxSerializedLine)
	}
	f, err := os.OpenFile(s.decisionsPath, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o640)
	if err != nil {
		return fmt.Errorf("open notifier decision store: %w", err)
	}
	defer f.Close()
	if _, err := f.Write(append(body, '\n')); err != nil {
		return fmt.Errorf("append notifier decision: %w", err)
	}
	if err := f.Sync(); err != nil {
		return fmt.Errorf("sync notifier decision: %w", err)
	}
	return nil
}

func (s *Store) RecordSuccess(now time.Time, sourceEventCount int, sink, minSeverity string, cooldown time.Duration) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	hb := Heartbeat{
		Version:           StoreVersion,
		LastSweepAt:       now.UTC().Format(time.RFC3339),
		LastSuccessAt:     now.UTC().Format(time.RFC3339),
		SourceEventCount:  sourceEventCount,
		ProcessedCount:    s.processedCount,
		DeliveredCount:    s.deliveredCount,
		SuppressedCount:   s.suppressedCount,
		LastSourceEventID: s.lastSourceEventID,
		BootstrapComplete: s.bootstrapComplete,
		Sink:              sink,
		MinSeverity:       minSeverity,
		Cooldown:          cooldown.String(),
	}
	if err := s.writeHeartbeat(hb); err != nil {
		return err
	}
	s.heartbeat = hb
	return nil
}

func (s *Store) RecordFailure(now time.Time, sourceEventCount int, sink, minSeverity string, cooldown time.Duration, cause error) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	hb := s.heartbeat
	hb.Version = StoreVersion
	hb.LastSweepAt = now.UTC().Format(time.RFC3339)
	hb.SourceEventCount = sourceEventCount
	hb.ProcessedCount = s.processedCount
	hb.DeliveredCount = s.deliveredCount
	hb.SuppressedCount = s.suppressedCount
	hb.LastSourceEventID = s.lastSourceEventID
	hb.BootstrapComplete = s.bootstrapComplete
	hb.Sink = sink
	hb.MinSeverity = minSeverity
	hb.Cooldown = cooldown.String()
	if cause != nil {
		hb.LastError = cause.Error()
	}
	if err := s.writeHeartbeat(hb); err != nil {
		return err
	}
	s.heartbeat = hb
	return nil
}

func (s *Store) writeHeartbeat(hb Heartbeat) error {
	body, err := json.Marshal(hb)
	if err != nil {
		return fmt.Errorf("encode notifier heartbeat: %w", err)
	}
	tmp := s.statusPath + ".tmp"
	if err := os.WriteFile(tmp, body, 0o640); err != nil {
		return fmt.Errorf("write notifier heartbeat: %w", err)
	}
	if err := os.Rename(tmp, s.statusPath); err != nil {
		return fmt.Errorf("publish notifier heartbeat: %w", err)
	}
	return nil
}
