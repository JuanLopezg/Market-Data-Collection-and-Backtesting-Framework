package main

import (
	"testing"
	"time"

	"control-dashboard-api/internal/alertstore"
)

func sourceAlert(service, event string) alertstore.Alert {
	return alertstore.Alert{ID: service + "-failure", Service: service, EventType: event,
		Status: "ACTIVE", Severity: "CRITICAL", Title: "Source failed", Detail: "read timeout"}
}

func TestSourceFailureRequiresConsecutiveSweepsAndResolvesImmediately(t *testing.T) {
	for _, alert := range []alertstore.Alert{
		sourceAlert("PostgreSQL", "SOURCE_CRITICAL"),
		sourceAlert("NATS", "SOURCE_DEGRADED"),
		sourceAlert("PortfolioRisk", "READ_MODEL_UNAVAILABLE"),
		sourceAlert("Execution", "READ_MODEL_UNAVAILABLE"),
		sourceAlert("Reconciliation", "READ_MODEL_UNAVAILABLE"),
		sourceAlert("Ledger", "LEDGER_READ_UNAVAILABLE"),
		{ID: "progress", Service: "DailyTradingProgress", EventType: "SOURCE_CRITICAL", Status: "ACTIVE",
			Severity: "CRITICAL", Detail: "Durable trading progress unavailable: authenticated PostgreSQL read failed."},
	} {
		t.Run(alert.Service, func(t *testing.T) {
			store, err := alertstore.Open(t.TempDir())
			if err != nil {
				t.Fatal(err)
			}
			d := newSourceDebouncer(nil)
			now := time.Date(2026, 10, 21, 12, 0, 1, 0, time.UTC)
			// One boundary failure followed by health produces no lifecycle events.
			for i, input := range [][]alertstore.Alert{{alert}, nil, {alert}, {alert}, nil, {alert}} {
				hb, err := store.Apply(now.Add(time.Duration(i)*15*time.Second), d.filter(input))
				if err != nil {
					t.Fatal(err)
				}
				wantEvents := []int64{0, 0, 0, 1, 2, 2}[i]
				wantActive := 0
				if i == 3 {
					wantActive = 1
				}
				if hb.EventCount != wantEvents || hb.ActiveCount != wantActive {
					t.Fatalf("sweep %d: %+v", i, hb)
				}
			}
		})
	}
}

func TestTradingAlertsAreNeverDebounced(t *testing.T) {
	for _, alert := range []alertstore.Alert{
		sourceAlert("Reconciliation", "RECONCILIATION_BLOCKED"),
		sourceAlert("Reconciliation", "RECONCILIATION_DRIFT"),
		sourceAlert("Ledger", "LEDGER_INTEGRITY_BLOCKED"),
		sourceAlert("SymbolMap", "SYMBOL_MAP_UNAVAILABLE"),
		sourceAlert("SymbolMap", "SCALE_MISMATCH"),
		sourceAlert("ExecutionState", "TRADING_PROCESS_UNAVAILABLE"),
		sourceAlert("HostResources", "SOURCE_CRITICAL"),
		{ID: "progress", Service: "DailyTradingProgress", EventType: "SOURCE_CRITICAL", Status: "ACTIVE",
			Detail: "Daily decision or plan application is missing/behind the expected dates."},
	} {
		d := newSourceDebouncer(nil)
		if got := d.filter([]alertstore.Alert{alert}); len(got) != 1 {
			t.Fatalf("genuine alert was suppressed: %+v", alert)
		}
	}
}

func TestSourceStreakSurvivesChangingSeverityAndDetail(t *testing.T) {
	d := newSourceDebouncer(nil)
	first := sourceAlert("PostgreSQL", "SOURCE_DEGRADED")
	second := sourceAlert("PostgreSQL", "SOURCE_CRITICAL")
	second.ID = "another-severity-id"
	second.Detail = "connection refused"
	if got := d.filter([]alertstore.Alert{first, first}); len(got) != 0 {
		t.Fatal("duplicate observations within one sweep confirmed a failure")
	}
	if got := d.filter([]alertstore.Alert{second}); len(got) != 1 || got[0].ID != second.ID {
		t.Fatal("severity/detail change reset a continuous source failure")
	}
}

func TestFailedSweepBreaksPendingStreakWithoutResolvingConfirmedAlerts(t *testing.T) {
	alert := sourceAlert("PostgreSQL", "SOURCE_CRITICAL")
	d := newSourceDebouncer(nil)
	d.filter([]alertstore.Alert{alert})
	d.interrupt()
	if got := d.filter([]alertstore.Alert{alert}); len(got) != 0 {
		t.Fatal("unreadable sweep counted as a consecutive source observation")
	}
	d.filter([]alertstore.Alert{alert})
	d.interrupt()
	if got := d.filter([]alertstore.Alert{alert}); len(got) != 1 {
		t.Fatal("confirmed source alert was suppressed after an unreadable sweep")
	}
}

func TestRestartPreservesConfirmedSourceFailure(t *testing.T) {
	dir := t.TempDir()
	store, err := alertstore.Open(dir)
	if err != nil {
		t.Fatal(err)
	}
	alert := sourceAlert("Ledger", "LEDGER_READ_UNAVAILABLE")
	now := time.Now().UTC()
	if _, err := store.Apply(now, []alertstore.Alert{alert}); err != nil {
		t.Fatal(err)
	}
	restarted, err := alertstore.Open(dir)
	if err != nil {
		t.Fatal(err)
	}
	d := newSourceDebouncer(alertstore.ReadSnapshot(dir, 1).Active)
	hb, err := restarted.Apply(now.Add(15*time.Second), d.filter([]alertstore.Alert{alert}))
	if err != nil || hb.ActiveCount != 1 || hb.EventCount != 1 {
		t.Fatalf("restart fabricated a recovery: %+v, %v", hb, err)
	}
	// An integrity failure with the same alert ID must be admitted immediately.
	alert.EventType = "LEDGER_INTEGRITY_BLOCKED"
	if got := d.filter([]alertstore.Alert{alert}); len(got) != 1 {
		t.Fatal("source debounce hid a ledger integrity failure")
	}
}
