package provider

import (
	"errors"
	"strings"
	"testing"
	"time"

	natsdiag "control-dashboard-api/internal/integration/nats"
	pgstore "control-dashboard-api/internal/integration/postgres"
	sourceprobe "control-dashboard-api/internal/integration/probe"
)

func TestExecutionStatePersistenceHealth(t *testing.T) {
	checkedAt := time.Date(2026, 10, 20, 12, 0, 0, 0, time.UTC)
	for _, tc := range []struct {
		name    string
		runtime pgstore.RuntimeSummary
		state   string
		ready   bool
		label   string
	}{
		{"fresh", pgstore.RuntimeSummary{Present: true, UpdatedAt: "2026-10-20T11:48:00.000Z"}, "HEALTHY", true, "Snapshot updated 12 min ago"},
		{"26 hour boundary", pgstore.RuntimeSummary{Present: true, UpdatedAt: "2026-10-19T10:00:00Z"}, "HEALTHY", true, "Snapshot updated 1560 min ago"},
		{"stale", pgstore.RuntimeSummary{Present: true, UpdatedAt: "2026-10-19T09:59:00Z"}, "WARN", false, "Snapshot updated 1561 min ago"},
		{"absent", pgstore.RuntimeSummary{}, "UNKNOWN", false, "Durable execution snapshot not present; start the runtime services to populate it"},
		{"invalid timestamp", pgstore.RuntimeSummary{Present: true, UpdatedAt: "invalid"}, "UNKNOWN", false, "Snapshot persistence age unavailable: invalid update timestamp"},
		{"future timestamp", pgstore.RuntimeSummary{Present: true, UpdatedAt: "2026-10-20T12:01:00Z"}, "UNKNOWN", false, "Snapshot update timestamp is in the future"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			service := buildExecutionStateHealth(tc.runtime, checkedAt)
			if service.Service != "ExecutionState durable state" || service.Health != tc.state || service.Ready != tc.ready || service.LagLabel != tc.label || service.LastEvent != tc.runtime.UpdatedAt {
				t.Fatalf("unexpected durable state service: %+v", service)
			}
			dependencies := buildDependencies(sourceprobe.Status{}, pgstore.OperationalSnapshot{Runtime: tc.runtime}, nil, natsdiag.MonitorStatus{}, nil, checkedAt, "SIMULATED")
			for _, dependency := range dependencies {
				if dependency.Component == "ExecutionState durable snapshot" {
					if dependency.State != tc.state || !strings.HasPrefix(dependency.Reason, tc.label) || dependency.LastCheck != "12:00:00 UTC" {
						t.Fatalf("unexpected durable snapshot dependency: %+v", dependency)
					}
					return
				}
			}
			t.Fatal("durable snapshot dependency missing")
		})
	}
}

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
			if got := buildTradingProgress(runtime, nil, true, true, now); got.State != tc.state {
				t.Fatalf("unexpected progress: %+v", got)
			}
		})
	}
	runtime.LastBarCloseTimestamp, runtime.LastExecutionTimestamp = 20261006, 20261007
	if got := buildTradingProgress(runtime, nil, true, true, now.Truncate(24*time.Hour).Add(10*time.Minute)); got.State != "WARN" {
		t.Fatalf("UTC grace not respected: %+v", got)
	}
	if got := buildTradingProgress(runtime, nil, true, true, now.Truncate(24*time.Hour).Add(30*time.Minute)); got.State != "CRITICAL" {
		t.Fatalf("UTC grace did not expire: %+v", got)
	}
	if got := buildTradingProgress(runtime, errors.New("read failed"), true, true, now); got.State != "CRITICAL" {
		t.Fatal("Database failure hidden")
	}
	if got := buildTradingProgress(runtime, nil, true, false, now); got.State != "UNKNOWN" {
		t.Fatal("Replay business clock compared to wall clock")
	}
	if got := buildTradingProgress(pgstore.RuntimeSummary{}, nil, true, true, now); got.State != "UNKNOWN" {
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
