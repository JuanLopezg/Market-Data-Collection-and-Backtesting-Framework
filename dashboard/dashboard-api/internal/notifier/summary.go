package notifier

import (
	"context"
	"fmt"
	"sort"
	"strings"

	"control-dashboard-api/internal/alertstore"
)

// Active-alert summaries (user request 2026-10-17).
//
// After a sweep delivers an alert change (opened, escalated or resolved) to a chat, the chat
// also gets the list of alerts still active in it: the critical group lists CRITICAL alerts,
// the normal chat the others (one combined list when there is no critical group). Only
// alerts that were reported and not resolved count, so a CRITICAL alert still inside its
// delay or one below the minimum severity is not listed. Each notifier (Watchdog, Kraken)
// lists its own source. The summary is silent and not journaled: it describes the state at
// that moment and a later change sends a new one.

// SummarySink is implemented by sinks that can post an active-alert summary.
type SummarySink interface {
	// SeparateCriticalChat reports whether CRITICAL alerts go to their own chat.
	SeparateCriticalChat() bool
	SendSummary(ctx context.Context, critical bool, text string) error
}

func (s *TelegramSink) SeparateCriticalChat() bool { return s.criticalChatID != "" }

func (s *TelegramSink) SendSummary(ctx context.Context, critical bool, text string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.now().Before(s.retryNotBefore) {
		return fmt.Errorf("Telegram summary skipped until %s after rate limiting", s.retryNotBefore.UTC().Format("15:04:05 UTC"))
	}
	chatID := s.chatID
	if critical && s.criticalChatID != "" {
		chatID = s.criticalChatID
	}
	_, err := s.sendMessage(ctx, chatID, text, true, "")
	return err
}

type activeAlert struct {
	Label    string
	Severity string
}

// activeReportedAlerts returns the alerts whose latest lifecycle event is not a resolution
// and that this notifier has reported. Daily portfolio summaries are reports, not alarms.
func (e *Engine) activeReportedAlerts(events []alertstore.LifecycleEvent) []activeAlert {
	latest := map[string]alertstore.LifecycleEvent{}
	for _, event := range events {
		latest[event.Alert.ID] = event
	}
	alerts := make([]activeAlert, 0, len(latest))
	for id, event := range latest {
		if event.Transition == "RESOLVED" || event.Alert.EventType == "DAILY_PORTFOLIO" {
			continue
		}
		if _, reported := e.store.LastDelivery(id); !reported {
			continue
		}
		alerts = append(alerts, activeAlert{Label: alertLabel(event.Alert),
			Severity: strings.ToUpper(strings.TrimSpace(event.Alert.Severity))})
	}
	sort.Slice(alerts, func(i, j int) bool { return alerts[i].Label < alerts[j].Label })
	return alerts
}

// summaryText lists the alerts of one chat: critical (the critical group), non-critical
// (the normal chat) or both when separate is false (a single chat).
func summaryText(source string, alerts []activeAlert, critical, separate bool) string {
	var labels []string
	criticalCount := 0
	for _, alert := range alerts {
		severity := strings.ToUpper(strings.TrimSpace(alert.Severity))
		isCritical := severity == "CRITICAL"
		if separate && isCritical != critical {
			continue
		}
		if isCritical {
			criticalCount++
		}
		labels = append(labels, fmt.Sprintf("%s %s", telegramSeverityMarker(severity), alert.Label))
	}
	kind := "Active alerts"
	if separate && critical {
		kind = "Critical alerts"
	}
	header := fmt.Sprintf("📋 %s · %s now: %d", strings.TrimSpace(source), kind, len(labels))
	if !separate && criticalCount > 0 {
		header += fmt.Sprintf(" (%d critical)", criticalCount)
	}
	if len(labels) == 0 {
		return header + "\n✅ All clear"
	}
	lines := []string{header}
	for _, label := range labels {
		lines = append(lines, label)
	}
	return truncateRunes(strings.Join(lines, "\n"), maxTelegramTextRunes)
}

// alertLabel names an alert in a few words a person recognises ("gateway-kraken down",
// "order rejected: NEARUSDT"); unknown types fall back to their short title.
func alertLabel(alert alertstore.Alert) string {
	service, asset := strings.TrimSpace(alert.Service), strings.TrimSpace(alert.Asset)
	with := func(text string) string {
		if asset == "" {
			return text
		}
		return asset + ": " + text
	}
	if service == "HostResources" {
		return "VPS CPU, RAM or disk high"
	}
	switch alert.EventType {
	case "TRADING_PROCESS_UNAVAILABLE":
		return service + " down"
	case "SOURCE_CRITICAL":
		return service + " unavailable"
	case "SOURCE_DEGRADED":
		return service + " degraded"
	case "CLOCK_UNSYNCHRONIZED":
		return "VPS clock not synchronized"
	case "ORDER_REJECT":
		return with("order rejected")
	case "ORDER_SKIPPED":
		return with("order skipped by the exchange")
	case "RECONCILIATION_BLOCKED":
		return "reconciliation blocked"
	case "RECONCILIATION_PENDING":
		return "reconciliation pending"
	case "RECONCILIATION_DRIFT":
		return "reconciliation drift"
	case "SYMBOL_MAP_SCALE_MISMATCH":
		return with("symbol map scale wrong")
	case "SYMBOL_MAP_SIGNAL_NOT_TRADEABLE":
		return with("signal not tradeable on Kraken")
	case "SYMBOL_MAP_STRATEGY_NOT_TRADEABLE":
		return with("not tradeable on Kraken")
	case "SYMBOL_MAP_TABLE_OUT_OF_DATE":
		return with("missing from the symbol map")
	case "SYMBOL_MAP_VENUE_NEW_CONTRACT":
		return with("new Kraken contract, refresh the symbol map")
	case "SYMBOL_MAP_VENUE_DELISTED":
		return with("delisted on Kraken, refresh the symbol map")
	case "SYMBOL_MAP_RANKING_NOT_TRADEABLE":
		return with("not listed on Kraken")
	case "SYMBOL_MAP_UNAVAILABLE":
		return "symbol map check unavailable"
	case "STALE_DATA":
		return "stale market data"
	case "CANDLE_GAPS":
		return "market data gaps"
	case "DATA_INTEGRITY":
		return "market data integrity issue"
	case "SIGNAL_CYCLE_NOT_ALIGNED":
		return "strategy behind the market cycle"
	case "WATCHDOG_STALE", "WATCHDOG_SWEEP_ERROR", "WATCHDOG_UNAVAILABLE":
		return "alert watchdog not working"
	case "KRAKEN_UNREADABLE":
		return "Kraken account unreadable"
	}
	title := strings.TrimSuffix(cleanTelegramField(alert.Title), " resolved")
	if title == "" {
		title = strings.ToLower(strings.ReplaceAll(alert.EventType, "_", " "))
	}
	if asset != "" && !strings.Contains(title, asset) {
		title = with(title)
	}
	return truncateRunes(title, 70)
}
