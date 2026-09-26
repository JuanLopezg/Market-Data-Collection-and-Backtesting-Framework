package provider

import (
	"errors"
	"testing"

	natsdiag "control-dashboard-api/internal/integration/nats"
	pgstore "control-dashboard-api/internal/integration/postgres"
	sourceprobe "control-dashboard-api/internal/integration/probe"
)

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
