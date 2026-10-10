package notifier

import (
	"context"
	"fmt"
	"strings"
	"time"

	"control-dashboard-api/internal/alertstore"
)

type Engine struct {
	store       *Store
	sink        Sink
	minSeverity string
	cooldown    time.Duration
	now         func() time.Time
}

type SweepStats struct {
	SourceEvents int
	Processed    int
	Delivered    int
	Suppressed   int
	Bootstrap    bool
}

func NewEngine(store *Store, sink Sink, minSeverity string, cooldown time.Duration) (*Engine, error) {
	if store == nil {
		return nil, fmt.Errorf("notifier store is nil")
	}
	if sink == nil {
		return nil, fmt.Errorf("notifier sink is nil")
	}
	minSeverity = strings.ToUpper(strings.TrimSpace(minSeverity))
	if severityRank(minSeverity) == 0 {
		return nil, fmt.Errorf("unsupported notifier minimum severity %q", minSeverity)
	}
	if cooldown < 0 {
		return nil, fmt.Errorf("notifier cooldown cannot be negative")
	}
	return &Engine{store: store, sink: sink, minSeverity: minSeverity, cooldown: cooldown, now: time.Now}, nil
}

func (e *Engine) Process(ctx context.Context, events []alertstore.LifecycleEvent) (SweepStats, error) {
	stats := SweepStats{SourceEvents: len(events), Bootstrap: !e.store.BootstrapComplete()}
	activeLatest := map[string]string{}
	if stats.Bootstrap {
		for _, event := range events {
			switch event.Transition {
			case "OPENED", "UPDATED":
				activeLatest[event.Alert.ID] = event.EventID
			case "RESOLVED":
				delete(activeLatest, event.Alert.ID)
			}
		}
	}

	for _, event := range events {
		select {
		case <-ctx.Done():
			return stats, ctx.Err()
		default:
		}
		if e.store.IsProcessed(event.EventID) {
			continue
		}

		action, reason := e.decide(event, stats.Bootstrap, activeLatest)
		decision := Decision{
			Version:          StoreVersion,
			DecisionID:       "decision:" + event.EventID,
			RecordedAt:       e.now().UTC().Format(time.RFC3339Nano),
			SourceEventID:    event.EventID,
			SourceRecordedAt: event.RecordedAt,
			AlertID:          event.Alert.ID,
			Transition:       event.Transition,
			Severity:         strings.ToUpper(strings.TrimSpace(event.Alert.Severity)),
			Service:          event.Alert.Service,
			EventType:        event.Alert.EventType,
			Title:            event.Alert.Title,
			Action:           action,
			Reason:           reason,
		}

		if action == "DELIVERED" {
			notificationID := "notify:" + event.EventID
			n := Notification{
				Version:          NotificationVersion,
				NotificationID:   notificationID,
				DeliveredAt:      e.now().UTC().Format(time.RFC3339Nano),
				SourceEventID:    event.EventID,
				SourceRecordedAt: event.RecordedAt,
				Transition:       event.Transition,
				AlertID:          event.Alert.ID,
				Severity:         strings.ToUpper(strings.TrimSpace(event.Alert.Severity)),
				Service:          event.Alert.Service,
				Account:          event.Alert.Account,
				EventType:        event.Alert.EventType,
				Title:            event.Alert.Title,
				Detail:           event.Alert.Detail,
				CorrelationID:    event.Alert.CorrelationID,
				LinkedContext:    event.Alert.LinkedContext,
				Reason:           reason,
			}
			if _, err := e.sink.Deliver(ctx, n); err != nil {
				return stats, fmt.Errorf("deliver %s: %w", notificationID, err)
			}
			decision.NotificationID = notificationID
			decision.Sink = e.sink.Name()
		}
		if err := e.store.AppendDecision(decision); err != nil {
			return stats, err
		}
		stats.Processed++
		if action == "DELIVERED" {
			stats.Delivered++
		} else {
			stats.Suppressed++
		}
	}
	if stats.Bootstrap {
		if err := e.store.MarkBootstrapComplete(e.now()); err != nil {
			return stats, err
		}
	}
	return stats, nil
}

func (e *Engine) decide(event alertstore.LifecycleEvent, bootstrap bool, activeLatest map[string]string) (string, string) {
	severity := strings.ToUpper(strings.TrimSpace(event.Alert.Severity))
	if bootstrap {
		if activeLatest[event.Alert.ID] != event.EventID {
			return "SUPPRESSED", "BOOTSTRAP_HISTORY"
		}
		if severityRank(severity) < severityRank(e.minSeverity) {
			return "SUPPRESSED", "BELOW_MIN_SEVERITY"
		}
		return "DELIVERED", "BOOTSTRAP_ACTIVE"
	}

	if event.Transition == "RESOLVED" {
		if _, ok := e.store.LastDelivery(event.Alert.ID); ok {
			return "DELIVERED", "RESOLVED_AFTER_NOTIFICATION"
		}
		return "SUPPRESSED", "RESOLVED_WITHOUT_PRIOR_NOTIFICATION"
	}
	if severityRank(severity) < severityRank(e.minSeverity) {
		return "SUPPRESSED", "BELOW_MIN_SEVERITY"
	}
	// The producer emits one persisted event per UTC day. Calendar rollover must
	// not lose a summary to the operational warning update cooldown.
	if event.Alert.EventType == "DAILY_PORTFOLIO" && severity == "INFO" {
		return "DELIVERED", "DAILY_SUMMARY"
	}
	last, exists := e.store.LastDelivery(event.Alert.ID)
	if !exists || event.Transition == "OPENED" {
		return "DELIVERED", "OPENED"
	}
	if severityRank(severity) > severityRank(last.Severity) {
		return "DELIVERED", "SEVERITY_ESCALATION"
	}
	currentAt, currentOK := parseEventTime(event.RecordedAt)
	lastAt, lastOK := parseEventTime(last.SourceRecordedAt)
	if !currentOK || !lastOK {
		return "DELIVERED", "COOLDOWN_TIME_UNAVAILABLE"
	}
	if currentAt.Sub(lastAt) >= e.cooldown {
		return "DELIVERED", "UPDATE_AFTER_COOLDOWN"
	}
	return "SUPPRESSED", "COOLDOWN"
}

func parseEventTime(raw string) (time.Time, bool) {
	t, err := time.Parse(time.RFC3339Nano, strings.TrimSpace(raw))
	if err != nil {
		return time.Time{}, false
	}
	return t.UTC(), true
}

func severityRank(raw string) int {
	switch strings.ToUpper(strings.TrimSpace(raw)) {
	case "INFO":
		return 1
	case "WARN":
		return 2
	case "CRITICAL":
		return 3
	default:
		return 0
	}
}
