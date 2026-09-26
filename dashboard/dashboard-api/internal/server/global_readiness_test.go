package server

import (
	"errors"
	"strings"
	"testing"
	"time"

	"control-dashboard-api/internal/provider"
)

func baselineReadinessInputs(now time.Time) globalReadinessInputs {
	in := globalReadinessInputs{
		ProviderHealth: provider.Health{Mode: "real", Name: "real-data-provider-step44"},
		Now:            now,
		FoundationOK:   true,
		VenuePublicOK:  true,
		VenueRulesOK:   true,
		RegistryOK:     true,
		LedgerOK:       true,
	}
	in.Infra.Postgres.State = "HEALTHY"
	in.Infra.NATS.State = "HEALTHY"
	in.Infra.Services = []struct {
		Service string `json:"service"`
	}{{Service: "ExecutionState durable state"}}
	in.Shell.DataState = "HEALTHY"
	in.Shell.DataDetail = "canonical market data healthy"
	in.Shell.Reconciliation = "PENDING"
	in.Pipeline.Proof = &struct {
		Status     string `json:"status"`
		Completion string `json:"completion"`
	}{Status: "PENDING", Completion: "INCOMPLETE_OR_IN_FLIGHT"}
	in.Alerts.WatchdogAvailable = true
	in.Alerts.DurableAlertHistoryAvailable = true
	in.Alerts.WatchdogState = "HEALTHY"
	in.Alerts.WatchdogLastSuccessAt = now.Add(-15 * time.Second).Format(time.RFC3339)
	in.Foundation = provider.VenueFoundation{
		FoundationReady:   true,
		Venue:             "HYPERLIQUID",
		TargetEnvironment: "TESTNET",
		GatewayMode:       "hyperliquid-dry-run",
		PrivateAuth:       "DISABLED",
		OrderRouting:      "DISABLED",
	}
	in.VenuePublic = provider.VenuePublicStatus{
		Connected: true, Venue: "HYPERLIQUID", TargetEnvironment: "TESTNET", UniverseCount: 200, MidCount: 3000,
	}
	in.VenueRules.Validated = true
	in.VenueRules.ValidatedRuleCount = 10
	in.VenueRules.SupportedMappingCount = 10
	in.Registry.Validated = true
	in.Registry.RegistryVersion = "test-registry"
	in.Registry.CurrentRankingCount = 50
	in.Registry.CurrentRankingRegisteredCount = 50
	in.Registry.CurrentStrategyCount = 20
	in.Registry.CurrentStrategyRegisteredCount = 20
	in.Registry.CurrentStrategyRoutableCount = 10
	in.Registry.ExecutionCoverageComplete = false
	in.Ledger.Validated = true
	in.Ledger.FoundationReady = true
	in.Ledger.TotalFillRows = 0
	in.Manual.ContractVersion = provider.ManualControlContractVersion
	in.Manual.RoutingContractReady = true
	in.Manual.HumanAuditAvailable = true
	in.Manual.RouteEnabled = false
	in.DemoAuth = true
	in.CookieSecure = false
	return in
}

func TestGlobalReadinessAllowsDashboardButNeverClaimsPrivateOrLiveReady(t *testing.T) {
	now := time.Date(2026, 9, 26, 12, 0, 0, 0, time.UTC)
	got := buildGlobalReadiness(baselineReadinessInputs(now))

	if got.Status != "VALIDATED_FAIL_CLOSED" || !got.ContractComplete || !got.SafeToContinueDashboard {
		t.Fatalf("unexpected dashboard readiness: %+v", got)
	}
	if got.PrivateTestnetReady || got.TradingReady || got.LiveReady {
		t.Fatalf("deferred private/live readiness must remain false: %+v", got)
	}
	if got.OrderRouting != "DISABLED" || got.ManualRouting != "DISABLED" {
		t.Fatalf("routing unexpectedly enabled: %+v", got)
	}
	if got.DeferredCount < 5 {
		t.Fatalf("expected explicit deferred contracts, got %d", got.DeferredCount)
	}
}

func TestGlobalReadinessCriticalAlertBlocksDashboardContinuation(t *testing.T) {
	now := time.Date(2026, 9, 26, 12, 0, 0, 0, time.UTC)
	in := baselineReadinessInputs(now)
	in.Alerts.ActiveCritical = 1

	got := buildGlobalReadiness(in)
	if got.SafeToContinueDashboard || got.Status != "BLOCKED" || got.BlockingCount == 0 {
		t.Fatalf("critical alert must block current phase: %+v", got)
	}
}

func TestGlobalReadinessStaleWatchdogBlocks(t *testing.T) {
	now := time.Date(2026, 9, 26, 12, 0, 0, 0, time.UTC)
	in := baselineReadinessInputs(now)
	in.Alerts.WatchdogLastSuccessAt = now.Add(-3 * time.Minute).Format(time.RFC3339)

	got := buildGlobalReadiness(in)
	if got.SafeToContinueDashboard {
		t.Fatalf("stale watchdog must block current phase")
	}
}

func TestGlobalReadinessSourceFailureBlocksWithoutInventingPass(t *testing.T) {
	now := time.Date(2026, 9, 26, 12, 0, 0, 0, time.UTC)
	in := baselineReadinessInputs(now)
	in.InfraErr = errors.New("read timeout")

	got := buildGlobalReadiness(in)
	if got.SafeToContinueDashboard || got.BlockingCount < 2 {
		t.Fatalf("source failure must remain fail-closed: %+v", got)
	}
}

func TestGlobalReadinessMarksOnlyKnownTransientReadBlockersRetryable(t *testing.T) {
	now := time.Date(2026, 9, 26, 12, 0, 0, 0, time.UTC)
	in := baselineReadinessInputs(now)
	in.ShellErr = errors.New("canonical market-data read timed out")
	got := buildGlobalReadiness(in)
	if got.BlockingCount == 0 || got.RetryableBlockingCount != got.BlockingCount {
		t.Fatalf("expected only retryable current blockers, got blocking=%d retryable=%d", got.BlockingCount, got.RetryableBlockingCount)
	}

	in = baselineReadinessInputs(now)
	in.Registry.Validated = false
	in.Registry.Error = "registry mapping divergence detected"
	got = buildGlobalReadiness(in)
	if got.RetryableBlockingCount != 0 {
		t.Fatalf("semantic registry divergence must not be retryable: %+v", got)
	}
}
func TestGlobalReadinessTreatsSQLiteBusyAsRetryableReadBlocker(t *testing.T) {
	now := time.Date(2026, 9, 26, 12, 0, 0, 0, time.UTC)
	in := baselineReadinessInputs(now)
	in.Registry.Validated = false
	in.Registry.Error = "canonical strategy universe unavailable: read-only SQLite query failed: database is locked (5)"

	got := buildGlobalReadiness(in)
	if got.BlockingCount == 0 || got.RetryableBlockingCount == 0 {
		t.Fatalf("SQLite busy/locked read blocker should be retryable but fail-closed: %+v", got)
	}
	if got.SafeToContinueDashboard {
		t.Fatalf("retryable blocker must not be converted into PASS: %+v", got)
	}
}

func TestGlobalReadinessTreatsManualPostgresTimeoutAsRetryable(t *testing.T) {
	now := time.Date(2026, 9, 26, 12, 0, 0, 0, time.UTC)
	in := baselineReadinessInputs(now)
	in.ManualErr = errors.New("read manual-control reference checkpoint: PostgreSQL query timed out after 2s")

	got := buildGlobalReadiness(in)
	if got.BlockingCount != 1 || got.RetryableBlockingCount != 1 {
		t.Fatalf("manual-control read timeout should be one retryable fail-closed blocker: blocking=%d retryable=%d %+v", got.BlockingCount, got.RetryableBlockingCount, got)
	}
	if got.SafeToContinueDashboard || got.ManualRouting != "UNKNOWN" {
		t.Fatalf("unverified manual route must remain fail-closed/UNKNOWN: %+v", got)
	}
}

func TestGlobalReadinessTreatsTransientNATSTransportFailureAsRetryable(t *testing.T) {
	now := time.Date(2026, 9, 26, 12, 0, 0, 0, time.UTC)
	in := baselineReadinessInputs(now)
	in.Infra.NATS.State = "CRITICAL"
	in.Infra.NATS.AckHealthLabel = "NATS TCP probe failed: dial tcp 172.18.0.4:4222: i/o timeout"

	got := buildGlobalReadiness(in)
	if got.BlockingCount != 1 || got.RetryableBlockingCount != 1 {
		t.Fatalf("transient NATS transport failure should be retryable but fail-closed: blocking=%d retryable=%d %+v", got.BlockingCount, got.RetryableBlockingCount, got)
	}
	if got.SafeToContinueDashboard {
		t.Fatalf("retryable NATS failure must not be converted into PASS: %+v", got)
	}
}

func TestGlobalReadinessDoesNotRetrySemanticNATSConfigurationFailure(t *testing.T) {
	now := time.Date(2026, 9, 26, 12, 0, 0, 0, time.UTC)
	in := baselineReadinessInputs(now)
	in.Infra.NATS.State = "CRITICAL"
	in.Infra.NATS.AckHealthLabel = "NATS source is not configured"

	got := buildGlobalReadiness(in)
	if got.BlockingCount != 1 || got.RetryableBlockingCount != 0 {
		t.Fatalf("NATS configuration failure must be a non-retryable blocker: blocking=%d retryable=%d %+v", got.BlockingCount, got.RetryableBlockingCount, got)
	}
}

func TestGlobalReadinessAllObservedTransientStep44BlockersRemainFailClosedButRetryable(t *testing.T) {
	now := time.Date(2026, 9, 26, 13, 36, 25, 0, time.UTC)
	in := baselineReadinessInputs(now)
	in.Shell.DataState = "BLOCKED"
	in.Shell.DataDetail = "Canonical market-data diagnostics are unavailable: read canonical market-data SQLite window: SQLite query timed out after 2s"
	in.ManualErr = errors.New("read manual-control reference checkpoint: PostgreSQL query timed out after 2s")
	in.Registry.Validated = false
	in.Registry.Error = "canonical market-data ranking unavailable: SQLite query timed out after 2s"
	in.VenueRules.Validated = false
	in.VenueRules.Error = "canonical strategy universe unavailable: read canonical market-data SQLite window: SQLite query timed out after 2s"
	in.Infra.NATS.State = "CRITICAL"
	in.Infra.NATS.AckHealthLabel = "NATS TCP probe failed: dial tcp 172.18.0.4:4222: connection refused"

	got := buildGlobalReadiness(in)
	if got.BlockingCount != 5 || got.RetryableBlockingCount != 5 {
		t.Fatalf("observed transient Step44 blockers should all remain fail-closed but retryable: blocking=%d retryable=%d blockers=%v", got.BlockingCount, got.RetryableBlockingCount, got.Blockers)
	}
	if got.SafeToContinueDashboard || got.Status != "BLOCKED" {
		t.Fatalf("retryable blockers must not be converted into a PASS: %+v", got)
	}
}

func TestGlobalReadinessRetriesOnlyTransientCriticalSourceAlerts(t *testing.T) {
	now := time.Date(2026, 9, 26, 12, 0, 0, 0, time.UTC)
	in := baselineReadinessInputs(now)
	in.Alerts.ActiveCritical = 1
	in.Alerts.Alerts = []readinessOperationalAlert{{
		Severity: "CRITICAL", Status: "ACTIVE", Service: "NATS", EventType: "SOURCE_CRITICAL",
		Detail: "NATS monitor request failed: dial tcp 172.18.0.4:8222: i/o timeout",
	}}

	got := buildGlobalReadiness(in)
	if got.BlockingCount != 1 || got.RetryableBlockingCount != 1 || got.SafeToContinueDashboard {
		t.Fatalf("transient critical source alert must remain blocked but retryable: %+v", got)
	}
	var found bool
	for _, req := range got.Requirements {
		if req.ID == "critical-alerts" {
			found = true
			if !req.Retryable || req.State != "BLOCKED" {
				t.Fatalf("critical-alerts should be retryable BLOCKED: %+v", req)
			}
			if !strings.Contains(req.Detail, "NATS/SOURCE_CRITICAL") {
				t.Fatalf("critical-alert detail should identify source alert: %q", req.Detail)
			}
		}
	}
	if !found {
		t.Fatal("critical-alerts requirement missing")
	}
}

func TestGlobalReadinessDoesNotRetrySemanticCriticalSourceAlert(t *testing.T) {
	now := time.Date(2026, 9, 26, 12, 0, 0, 0, time.UTC)
	in := baselineReadinessInputs(now)
	in.Alerts.ActiveCritical = 1
	in.Alerts.Alerts = []readinessOperationalAlert{{
		Severity: "CRITICAL", Status: "ACTIVE", Service: "NATS", EventType: "SOURCE_CRITICAL",
		Detail: "NATS source is not configured",
	}}

	got := buildGlobalReadiness(in)
	if got.BlockingCount != 1 || got.RetryableBlockingCount != 0 || got.SafeToContinueDashboard {
		t.Fatalf("semantic critical source alert must remain non-retryable: %+v", got)
	}
}

func TestGlobalReadinessDoesNotRetryMixedCriticalAlerts(t *testing.T) {
	now := time.Date(2026, 9, 26, 12, 0, 0, 0, time.UTC)
	in := baselineReadinessInputs(now)
	in.Alerts.ActiveCritical = 2
	in.Alerts.Alerts = []readinessOperationalAlert{
		{Severity: "CRITICAL", Status: "ACTIVE", Service: "NATS", EventType: "SOURCE_CRITICAL", Detail: "i/o timeout"},
		{Severity: "CRITICAL", Status: "ACTIVE", Service: "Reconciliation", EventType: "RECONCILIATION_BLOCKED", Detail: "position mismatch"},
	}

	got := buildGlobalReadiness(in)
	if got.BlockingCount != 1 || got.RetryableBlockingCount != 0 {
		t.Fatalf("mixed semantic+transient critical alerts must not be retryable: %+v", got)
	}
}

func TestGlobalReadinessUserSnapshotFiveTransientBlockersAreAllRetryable(t *testing.T) {
	now := time.Date(2026, 9, 26, 13, 47, 25, 0, time.UTC)
	in := baselineReadinessInputs(now)
	in.Shell.DataState = "BLOCKED"
	in.Shell.DataDetail = "Canonical market-data diagnostics are unavailable: read canonical market-data SQLite window: SQLite query timed out after 2s"
	in.PipelineErr = errors.New("read durable pipeline checkpoint: read-only PostgreSQL query failed: psql: timeout expired")
	in.VenueRules.Validated = false
	in.VenueRules.Error = "canonical strategy universe unavailable: read canonical market-data SQLite window: SQLite query timed out after 2s"
	in.Registry.Validated = false
	in.Registry.Error = "canonical market-data ranking unavailable: SQLite query timed out after 2s"
	in.Alerts.ActiveCritical = 1
	in.Alerts.Alerts = []readinessOperationalAlert{{
		Severity: "CRITICAL", Status: "ACTIVE", Service: "NATS", EventType: "SOURCE_CRITICAL",
		Detail: "NATS monitor request failed: i/o timeout",
	}}

	got := buildGlobalReadiness(in)
	if got.BlockingCount != 5 || got.RetryableBlockingCount != 5 || got.SafeToContinueDashboard {
		t.Fatalf("expected exact fail-closed 5/5 retryable snapshot, got blocking=%d retryable=%d safe=%v %+v", got.BlockingCount, got.RetryableBlockingCount, got.SafeToContinueDashboard, got)
	}
}
