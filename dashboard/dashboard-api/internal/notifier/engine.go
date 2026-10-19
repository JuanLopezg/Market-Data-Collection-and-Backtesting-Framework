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
	// criticalDelay holds back a new CRITICAL alert until it has been active this long;
	// one resolved earlier (a restart, a brief outage) is never sent. 0 sends at once.
	criticalDelay time.Duration
	// summarySource names the active-alert summaries (summary.go); empty = no summaries.
	summarySource string
	now           func() time.Time
}

type SweepStats struct {
	SourceEvents int
	Processed    int
	Delivered    int
	Suppressed   int
	Deferred     int
	Bootstrap    bool
	Summaries    int
	SummaryError string // a failed summary never fails the sweep; the next change retries
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

// SetSummarySource enables active-alert summaries named after the alert source ("Watchdog",
// "Kraken") when the sink supports them.
func (e *Engine) SetSummarySource(source string) { e.summarySource = strings.TrimSpace(source) }

func (e *Engine) SetCriticalDelay(delay time.Duration) error {
	if delay < 0 {
		return fmt.Errorf("notifier critical delay cannot be negative")
	}
	e.criticalDelay = delay
	return nil
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

	// Each sweep reads the whole lifecycle log, so a deferred event is simply left
	// unprocessed and decided again on a later sweep.
	deferredAlerts := map[string]bool{}
	summarySink, summaries := e.sink.(SummarySink)
	summaries = summaries && e.summarySource != ""
	changedChats := map[bool]bool{} // true: the critical group (when it is a separate chat)
	for index, event := range events {
		select {
		case <-ctx.Done():
			return stats, ctx.Err()
		default:
		}
		if e.store.IsProcessed(event.EventID) {
			continue
		}

		var action, reason string
		if !stats.Bootstrap {
			action, reason = e.criticalDelayDecision(events, index, deferredAlerts)
		}
		if action == "DEFERRED" {
			deferredAlerts[event.Alert.ID] = true
			stats.Deferred++
			continue
		}
		if action == "" {
			action, reason = e.decide(event, stats.Bootstrap, activeLatest)
		}
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
			if summaries && event.Alert.EventType != "DAILY_PORTFOLIO" {
				changedChats[summarySink.SeparateCriticalChat() && decision.Severity == "CRITICAL"] = true
			}
		} else {
			stats.Suppressed++
		}
	}
	if stats.Bootstrap {
		if err := e.store.MarkBootstrapComplete(e.now()); err != nil {
			return stats, err
		}
	}
	if len(changedChats) > 0 {
		active := e.activeReportedAlerts(events)
		separate := summarySink.SeparateCriticalChat()
		for _, critical := range []bool{true, false} {
			if !changedChats[critical] {
				continue
			}
			if err := summarySink.SendSummary(ctx, critical, summaryText(e.summarySource, active, critical, separate)); err != nil {
				stats.SummaryError = err.Error()
				continue
			}
			stats.Summaries++
		}
	}
	return stats, nil
}

// criticalDelayDecision applies the critical delay to a not yet reported alert. It
// returns "" when the normal decision applies. Later events of a deferred alert wait
// with it, so the alert is reported in its original order.
func (e *Engine) criticalDelayDecision(events []alertstore.LifecycleEvent, index int, deferredAlerts map[string]bool) (string, string) {
	event := events[index]
	if event.Transition == "RESOLVED" || e.criticalDelay <= 0 {
		return "", ""
	}
	if _, delivered := e.store.LastDelivery(event.Alert.ID); delivered {
		return "", "" // Already reported: updates follow the cooldown rules.
	}
	if deferredAlerts[event.Alert.ID] {
		return "DEFERRED", ""
	}
	if strings.ToUpper(strings.TrimSpace(event.Alert.Severity)) != "CRITICAL" {
		return "", ""
	}
	for _, later := range events[index+1:] {
		if later.Alert.ID == event.Alert.ID && later.Transition == "RESOLVED" {
			return "SUPPRESSED", "RESOLVED_WITHIN_CRITICAL_DELAY"
		}
	}
	// Measure from the start of the active episode, not from its latest update.
	activeSince := event.RecordedAt
	for previous := index - 1; previous >= 0; previous-- {
		if events[previous].Alert.ID != event.Alert.ID {
			continue
		}
		if events[previous].Transition == "RESOLVED" {
			break
		}
		activeSince = events[previous].RecordedAt
	}
	since, ok := parseEventTime(activeSince)
	if !ok {
		return "", "" // An unreadable time never hides a critical alert.
	}
	if e.now().Sub(since) < e.criticalDelay {
		return "DEFERRED", ""
	}
	return "", ""
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
	case "ALERT":
		return 3
	case "CRITICAL":
		return 4
	default:
		return 0
	}
}
