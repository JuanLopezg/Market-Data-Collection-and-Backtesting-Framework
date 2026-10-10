package provider

import (
	"errors"
	"strings"
	"testing"
	"time"
)

func TestShellClockWarningMatchesObservationAndKeepsTradingDisabled(t *testing.T) {
	infra, recon, market, alerts := healthyShellFixtures()
	infra.VPS.ClockObserved, infra.VPS.ClockSynced = true, true
	got := buildRealShellStatus("LIVE", infra, recon, nil, market, nil, alerts, time.Now())
	if strings.Contains(strings.Join(got.Warnings, " "), "Host clock") || got.TradingEnabled || got.Readiness == "READY" {
		t.Fatal("Clock evidence falsely warned or granted trading readiness")
	}
	infra.VPS.ClockSynced = false
	got = buildRealShellStatus("LIVE", infra, recon, nil, market, nil, alerts, time.Now())
	if !strings.Contains(strings.Join(got.Warnings, " "), "Host clock is not synchronized") {
		t.Fatal("Unsynchronized clock not reported")
	}
}

func healthyShellFixtures() (realInfrastructure, realReconciliationData, realMarketData, realAlertsAuditData) {
	infra := realInfrastructure{
		Postgres: realPostgres{State: "HEALTHY", Connected: true},
		NATS:     realNATS{State: "HEALTHY", Connected: true},
		Services: []realService{{Service: "ExecutionState durable state", Health: "UNKNOWN"}},
	}
	recon := realReconciliationData{Status: "CLEAN", ComparisonAvailable: true, EvidenceFresh: true}
	market := realMarketData{TotalAssets: 20, HealthyAssets: 20, SignalCycleAligned: true}
	alerts := realAlertsAuditData{ActiveCritical: 1, ActiveWarnings: 2}
	return infra, recon, market, alerts
}

func TestBuildRealShellStatusHealthyVerifiedSourcesStayDegradedFailClosed(t *testing.T) {
	infra, recon, market, alerts := healthyShellFixtures()
	got := buildRealShellStatus("LIVE", infra, recon, nil, market, nil, alerts, time.Date(2026, 9, 25, 8, 0, 0, 0, time.UTC))
	if got.Readiness != "DEGRADED" {
		t.Fatalf("readiness = %q, want DEGRADED", got.Readiness)
	}
	if got.ExchangeConnected {
		t.Fatal("fresh exchange evidence must not be promoted to connectivity")
	}
	if got.ExchangeState != "EVIDENCE FRESH" {
		t.Fatalf("exchangeState = %q", got.ExchangeState)
	}
	if !got.DataHealthy || got.DataState != "HEALTHY" {
		t.Fatalf("unexpected data state: healthy=%v state=%q", got.DataHealthy, got.DataState)
	}
	if got.TradingEnabled || got.TradingState != "UNVERIFIED" {
		t.Fatalf("unexpected trading state: enabled=%v state=%q", got.TradingEnabled, got.TradingState)
	}
	if got.AlertCount != 3 || got.CriticalAlertCount != 1 || got.WarningAlertCount != 2 {
		t.Fatalf("unexpected alert counts: %+v", got)
	}
	if !strings.Contains(got.ReadinessDetail, "READY is not asserted") {
		t.Fatalf("readiness detail = %q", got.ReadinessDetail)
	}
}

func TestBuildRealShellStatusHardDependencyPauses(t *testing.T) {
	infra, recon, market, alerts := healthyShellFixtures()
	infra.Postgres.State = "CRITICAL"
	got := buildRealShellStatus("LIVE", infra, recon, nil, market, nil, alerts, time.Now())
	if got.Readiness != "PAUSED" || got.TradingState != "BLOCKED" {
		t.Fatalf("unexpected hard-block state: readiness=%q trading=%q", got.Readiness, got.TradingState)
	}
	if len(got.Blockers) == 0 || !strings.Contains(got.Blockers[0], "PostgreSQL") {
		t.Fatalf("blockers = %#v", got.Blockers)
	}
}

func TestBuildRealShellStatusReconciliationBlockedPauses(t *testing.T) {
	infra, recon, market, alerts := healthyShellFixtures()
	recon.Status = "BLOCKED"
	got := buildRealShellStatus("LIVE", infra, recon, nil, market, nil, alerts, time.Now())
	if got.Readiness != "PAUSED" {
		t.Fatalf("readiness = %q, want PAUSED", got.Readiness)
	}
	if !strings.Contains(strings.Join(got.Blockers, " | "), "reconciliation") {
		t.Fatalf("blockers = %#v", got.Blockers)
	}
}

func TestBuildRealShellStatusMarketReadFailurePauses(t *testing.T) {
	infra, recon, market, alerts := healthyShellFixtures()
	got := buildRealShellStatus("LIVE", infra, recon, nil, market, errors.New("sqlite unavailable"), alerts, time.Now())
	if got.Readiness != "PAUSED" || got.DataState != "BLOCKED" || got.DataHealthy {
		t.Fatalf("unexpected market failure state: %+v", got)
	}
}
