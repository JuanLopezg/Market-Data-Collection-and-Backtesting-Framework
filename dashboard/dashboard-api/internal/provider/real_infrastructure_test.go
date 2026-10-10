package provider

import (
	"errors"
	"testing"
	"time"

	natsdiag "control-dashboard-api/internal/integration/nats"
	pgstore "control-dashboard-api/internal/integration/postgres"
	sourceprobe "control-dashboard-api/internal/integration/probe"
)

func TestDailyTradingProgressUsesBusinessDatesNotRecentWritesOrFills(t *testing.T) {
	now := time.Date(2026, 10, 8, 12, 0, 0, 0, time.UTC)
	runtime := pgstore.RuntimeSummary{Present: true, LastBarCloseTimestamp: 20261007, LastExecutionTimestamp: 20261008, UpdatedAt: now.Format(time.RFC3339)}
	for _, tc := range []struct {
		name           string
		decision, plan uint64
		state          string
	}{
		{"empty daily plan is valid", 20261007, 20261008, "HEALTHY"},
		{"recent write cannot hide stalled decisions", 20261006, 20261007, "CRITICAL"},
		{"plan has not been applied", 20261007, 20261007, "CRITICAL"},
		{"future unfinished decision", 20261008, 20261008, "CRITICAL"},
		{"future plan", 20261007, 20261009, "CRITICAL"},
		{"absent dates", 0, 0, "CRITICAL"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			runtime.LastBarCloseTimestamp, runtime.LastExecutionTimestamp = tc.decision, tc.plan
			if got := buildTradingProgress(runtime, nil, true, "PAPER", now); got.State != tc.state {
				t.Fatalf("unexpected progress: %+v", got)
			}
		})
	}
	runtime.LastBarCloseTimestamp, runtime.LastExecutionTimestamp = 20261006, 20261007
	if got := buildTradingProgress(runtime, nil, true, "PAPER", now.Truncate(24*time.Hour).Add(10*time.Minute)); got.State != "WARN" {
		t.Fatalf("UTC grace not respected: %+v", got)
	}
	if got := buildTradingProgress(runtime, nil, true, "PAPER", now.Truncate(24*time.Hour).Add(30*time.Minute)); got.State != "CRITICAL" {
		t.Fatalf("UTC grace did not expire: %+v", got)
	}
	if got := buildTradingProgress(runtime, errors.New("read failed"), true, "PAPER", now); got.State != "CRITICAL" {
		t.Fatal("Database failure hidden")
	}
	if got := buildTradingProgress(runtime, nil, true, "LIVE", now); got.State != "UNKNOWN" {
		t.Fatal("Non-PAPER business clock compared to wall clock")
	}
	if got := buildTradingProgress(pgstore.RuntimeSummary{}, nil, true, "PAPER", now); got.State != "UNKNOWN" {
		t.Fatal("Missing first snapshot fabricated progress")
	}
}

func TestBuildPostgresHealthWithRuntime(t *testing.T) {
	source := sourceprobe.Result{Configured: true, Reachable: true, Detail: "ok"}
	snapshot := pgstore.OperationalSnapshot{
		Diagnostics: pgstore.Diagnostics{ActiveConnections: 3, MaxConnections: 100, DatabaseSizeBytes: 1024 * 1024, QueryLatencyMs: 2.34},
		Runtime:     pgstore.RuntimeSummary{Present: true, SchemaVersion: 1, UpdatedAt: "2026-09-24T16:00:00.000Z", AccountCash: 100000, PositionCount: 2, TrackedOrderCount: 1, FillRows: 4},
	}
	health := buildPostgresHealth(source, snapshot, nil)
	if !health.Connected || health.State != "HEALTHY" || health.ActiveConnections != 3 {
		t.Fatalf("unexpected postgres health: %+v", health)
	}
}

func TestBuildPostgresHealthAuthFailureIsCritical(t *testing.T) {
	source := sourceprobe.Result{Configured: true, Reachable: true, Detail: "tcp ok"}
	health := buildPostgresHealth(source, pgstore.OperationalSnapshot{}, errors.New("authentication failed"))
	if health.Connected || health.State != "CRITICAL" {
		t.Fatalf("expected critical authenticated-read failure, got %+v", health)
	}
}

func TestBuildNATSHealthFromMonitor(t *testing.T) {
	source := sourceprobe.Result{Configured: true, Reachable: true, Detail: "tcp ok"}
	monitor := natsdiag.MonitorStatus{Reachable: true, Streams: 2, Consumers: 5, Messages: 10, Bytes: 2048}
	health := buildNATSHealth(source, monitor, nil, true)
	if !health.Connected || health.State != "HEALTHY" || health.Streams != 2 || health.Consumers != 5 {
		t.Fatalf("unexpected NATS health: %+v", health)
	}
}
