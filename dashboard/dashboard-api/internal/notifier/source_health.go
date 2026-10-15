package notifier

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"time"
)

// SourceFailure contains only operator-safe categories, never producer errors or paths.
type SourceFailure string

const (
	SourceMissing       SourceFailure = "missing"
	SourceStale         SourceFailure = "stale"
	SourceAhead         SourceFailure = "ahead of clock"
	SourceError         SourceFailure = "source error"
	sourceHealthVersion               = "notifier-source-health-v1"
)

var sourceNamePattern = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9 ._-]{0,79}$`)

func ValidSourceName(name string) bool { return sourceNamePattern.MatchString(name) }

type sourceHealthState struct {
	Version    string        `json:"version"`
	Name       string        `json:"name"`
	Sequence   uint64        `json:"sequence"`
	Since      time.Time     `json:"since,omitempty"`
	Reason     SourceFailure `json:"reason,omitempty"`
	OpenedAt   time.Time     `json:"openedAt,omitempty"`
	ResolvedAt time.Time     `json:"resolvedAt,omitempty"`
}

// SourceMonitor owns a separate durable incident; it must not bootstrap or replay
// the producer's lifecycle while that producer is unhealthy. One instance per store.
type SourceMonitor struct {
	store        *Store
	sink         Sink
	warningAfter time.Duration
	state        sourceHealthState
}

func NewSourceMonitor(store *Store, sink Sink, name string, warningAfter time.Duration) (*SourceMonitor, error) {
	if store == nil || sink == nil || !ValidSourceName(name) || warningAfter <= 0 {
		return nil, errors.New("invalid notifier source monitor configuration")
	}
	m := &SourceMonitor{store: store, sink: sink, warningAfter: warningAfter,
		state: sourceHealthState{Version: sourceHealthVersion, Name: name}}
	f, err := os.Open(filepath.Join(store.dir, "source-health.json"))
	if errors.Is(err, os.ErrNotExist) {
		return m, nil
	}
	if err != nil {
		return nil, errors.New("read notifier source health state failed")
	}
	defer f.Close()
	body, err := io.ReadAll(io.LimitReader(f, 8193))
	if err != nil || len(body) > 8192 || json.Unmarshal(body, &m.state) != nil {
		return nil, errors.New("invalid notifier source health state")
	}
	s := m.state
	if s.Version != sourceHealthVersion || s.Name != name || !validSourceFailure(s.Reason) ||
		(s.Since.IsZero() && (s.Reason != "" || !s.OpenedAt.IsZero() || !s.ResolvedAt.IsZero())) ||
		(!s.Since.IsZero() && (s.Reason == "" || s.Sequence == 0)) ||
		(!s.ResolvedAt.IsZero() && s.OpenedAt.IsZero()) {
		return nil, errors.New("invalid notifier source health state or source identity")
	}
	return m, nil
}

func validSourceFailure(reason SourceFailure) bool {
	return reason == "" || reason == SourceMissing || reason == SourceStale || reason == SourceAhead || reason == SourceError
}

// Observe persists transitions before delivery. Stable IDs let the existing sink
// receipt recover a crash between remote acceptance and the decision append.
func (m *SourceMonitor) Observe(ctx context.Context, now time.Time, reason SourceFailure) error {
	if !validSourceFailure(reason) {
		return errors.New("invalid source failure category")
	}
	now = now.UTC()
	s := m.state
	// Finish a recovery left pending by an earlier failed delivery before starting
	// another incident. This retains distinct incidents even across a restart.
	if !s.ResolvedAt.IsZero() {
		if err := m.flush(ctx, now); err != nil {
			return err
		}
		s = m.state
	}
	if reason == "" {
		if s.Since.IsZero() {
			return nil
		}
		if s.OpenedAt.IsZero() {
			return m.save(sourceHealthState{Version: sourceHealthVersion, Name: s.Name, Sequence: s.Sequence})
		}
		s.ResolvedAt = now
	} else {
		if s.Since.IsZero() {
			s.Sequence++
			s.Since = now
		}
		s.Reason = reason
		if s.OpenedAt.IsZero() && now.Sub(s.Since) >= m.warningAfter {
			s.OpenedAt = now
		}
	}
	if s != m.state {
		if err := m.save(s); err != nil {
			return err
		}
	}
	return m.flush(ctx, now)
}

func (m *SourceMonitor) flush(ctx context.Context, now time.Time) error {
	s := m.state
	if s.OpenedAt.IsZero() {
		return nil
	}
	if err := m.deliver(ctx, now, "OPENED", s.OpenedAt); err != nil {
		return err
	}
	if s.ResolvedAt.IsZero() {
		return nil
	}
	if err := m.deliver(ctx, now, "RESOLVED", s.ResolvedAt); err != nil {
		return err
	}
	return m.save(sourceHealthState{Version: sourceHealthVersion, Name: s.Name, Sequence: s.Sequence})
}

func (m *SourceMonitor) deliver(ctx context.Context, now time.Time, transition string, at time.Time) error {
	s := m.state
	alertID := "notifier-source:" + s.Name
	eventID := fmt.Sprintf("%s:%d:%s", alertID, s.Sequence, transition)
	if m.store.IsProcessed(eventID) {
		return nil
	}
	title := "Alert source stopped"
	detail := fmt.Sprintf("Source: %s. Unhealthy since %s. Reason: %s.", s.Name, s.Since.Format(time.RFC3339), s.Reason)
	if transition == "RESOLVED" {
		title += " resolved"
		detail += " Healthy heartbeat observed at " + at.Format(time.RFC3339) + "."
	}
	n := Notification{Version: NotificationVersion, NotificationID: "notify:" + eventID,
		SourceEventID: eventID, SourceRecordedAt: at.Format(time.RFC3339Nano),
		DeliveredAt: now.Format(time.RFC3339Nano), AlertID: alertID, Severity: "WARN",
		Transition: transition, Service: "AlertNotifier", Account: s.Name,
		EventType: "ALERT_SOURCE_STOPPED", Title: title, Detail: detail, Reason: "SOURCE_HEALTH"}
	if _, err := m.sink.Deliver(ctx, n); err != nil {
		return fmt.Errorf("deliver source health notification: %w", err)
	}
	return m.store.AppendDecision(Decision{SourceEventID: eventID, SourceRecordedAt: n.SourceRecordedAt,
		RecordedAt: n.DeliveredAt, AlertID: alertID, Transition: transition, Severity: "WARN",
		Service: n.Service, EventType: n.EventType, Title: title, Action: "DELIVERED",
		Reason: n.Reason, NotificationID: n.NotificationID, Sink: m.sink.Name()})
}

func (m *SourceMonitor) save(state sourceHealthState) error {
	body, err := json.Marshal(state)
	if err != nil {
		return err
	}
	path := filepath.Join(m.store.dir, "source-health.json")
	f, err := os.OpenFile(path+".tmp", os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o640)
	if err != nil {
		return errors.New("write notifier source health state failed")
	}
	_, writeErr := f.Write(body)
	syncErr := f.Sync()
	closeErr := f.Close()
	if writeErr != nil || syncErr != nil || closeErr != nil {
		return errors.New("sync notifier source health state failed")
	}
	if err := os.Rename(path+".tmp", path); err != nil {
		return errors.New("publish notifier source health state failed")
	}
	m.state = state
	return nil
}
