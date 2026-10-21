package main

import (
	"strings"

	"control-dashboard-api/internal/alertstore"
)

// Only the watchdog uses this filter: API source diagnostics remain immediate.
// Two consecutive completed sweeps confirm a source failure. A healthy sweep
// clears its streak and resolves an admitted alert without a recovery delay.
type sourceDebouncer struct {
	streaks map[string]int
}

func newSourceDebouncer(active map[string]alertstore.ActiveAlert) *sourceDebouncer {
	d := &sourceDebouncer{streaks: make(map[string]int)}
	// Persisted active failures are already confirmed. On restart, do not
	// fabricate a recovery by suppressing their first observation again.
	for _, activeAlert := range active {
		if key := sourceFailureKey(activeAlert.Alert); key != "" {
			d.streaks[key] = 2
		}
	}
	return d
}

func (d *sourceDebouncer) filter(alerts []alertstore.Alert) []alertstore.Alert {
	next := make(map[string]int)
	result := make([]alertstore.Alert, 0, len(alerts))
	for _, alert := range alerts {
		if alert.Status != "ACTIVE" {
			continue
		}
		key := sourceFailureKey(alert)
		if key == "" {
			result = append(result, alert)
			continue
		}
		// Count sweeps, not duplicate alerts within a sweep. Severity and
		// changing timeout text must not reset a continuous source failure.
		count := min(d.streaks[key]+1, 2)
		next[key] = count
		if count >= 2 {
			result = append(result, alert)
		}
	}
	d.streaks = next
	return result
}

// An unreadable/undecodable sweep is not a second observation of a pending
// source failure. Confirmed alerts remain active in the durable store.
func (d *sourceDebouncer) interrupt() {
	for key, count := range d.streaks {
		if count < 2 {
			delete(d.streaks, key)
		}
	}
}

func sourceFailureKey(alert alertstore.Alert) string {
	switch alert.EventType {
	case "READ_MODEL_UNAVAILABLE", "LEDGER_READ_UNAVAILABLE":
		return alert.ID + ":" + alert.EventType
	case "SOURCE_CRITICAL", "SOURCE_DEGRADED":
		switch alert.Service {
		case "PostgreSQL", "NATS":
			return "source:" + alert.Service
		case "DailyTradingProgress":
			// Late/missing business cycles are genuine trading alerts. Only
			// inability to read the evidence gets source-failure debouncing.
			if strings.Contains(alert.Detail, "authenticated PostgreSQL read failed") {
				return "source:" + alert.Service
			}
		}
	}
	return ""
}
