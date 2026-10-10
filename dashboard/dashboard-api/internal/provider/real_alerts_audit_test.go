package provider

import (
	"control-dashboard-api/internal/alertack"
	"errors"
	"strings"
	"testing"
	"time"

	"control-dashboard-api/internal/alertstore"
	"control-dashboard-api/internal/manualaudit"
)

func TestOperationalAlertsUseFreshProcessClockAndDailyProgressEvidence(t *testing.T) {
	infra := realInfrastructure{LastUpdated: "12:00:00 UTC", TelemetryObservedAt: "2026-10-08T12:00:00Z",
		VPS:             realVPS{State: "CRITICAL", ClockObserved: true, ClockSynced: false, DiskPct: 96},
		Services:        []realService{{Service: "PortfolioRisk", ProcessState: "MISSING"}},
		TradingProgress: &realTradingProgress{State: "CRITICAL", Detail: "Daily decision is stale"}}
	read := func() realAlertsAuditData {
		return buildRealAlertsAudit(infra, realReconciliationData{}, nil, realExecutionData{}, nil, realMarketData{}, nil, realRiskData{}, nil)
	}
	got := read()
	for _, id := range []string{"derived-hostresources-critical", "derived-host-clock-unsynchronized", "derived-process-PortfolioRisk", "derived-dailytradingprogress-critical"} {
		found := false
		for _, alert := range got.Alerts {
			found = found || alert.ID == id
		}
		if !found {
			t.Fatalf("Missing operational alert %s", id)
		}
	}
	// Unsupported or stale observations must not invent an outage/clock failure.
	infra.TelemetryObservedAt = ""
	infra.TradingProgress.State = "UNKNOWN"
	for _, alert := range read().Alerts {
		if strings.HasPrefix(alert.ID, "derived-host") || strings.HasPrefix(alert.ID, "derived-process-") || strings.HasPrefix(alert.ID, "derived-dailytradingprogress-") {
			t.Fatalf("Unavailable observation became an active alert: %+v", alert)
		}
	}
	// No-order daily success and observed process/NTP recovery clear these conditions.
	infra.TelemetryObservedAt = "2026-10-08T12:01:00Z"
	infra.VPS.State, infra.VPS.ClockSynced = "HEALTHY", true
	infra.Services[0].ProcessState = "RUNNING"
	infra.TradingProgress.State = "HEALTHY"
	for _, alert := range read().Alerts {
		if strings.HasPrefix(alert.ID, "derived-host") || strings.HasPrefix(alert.ID, "derived-process-") || strings.HasPrefix(alert.ID, "derived-dailytradingprogress-") {
			t.Fatalf("Recovered condition remained active: %+v", alert)
		}
	}
}

func TestBuildRealAlertsAuditDerivesCurrentConditionsWithoutInventingPersistence(t *testing.T) {
	infra := realInfrastructure{
		LastUpdated: "08:00:00 UTC",
		Postgres:    realPostgres{State: "CRITICAL", PersistenceState: "unreachable"},
		NATS:        realNATS{State: "HEALTHY", AckHealthLabel: "ok"},
	}
	recon := realReconciliationData{Status: "BLOCKED", LastChecked: "2026-09-25 07:59:00 UTC", SourceNote: "mismatch"}
	exec := realExecutionData{SourceUpdatedAt: "2026-09-25T07:58:00Z", Rejects: []realExecutionReject{{OrderID: "7", Asset: "BTCUSDT", Reason: "venue rejected"}}}
	market := realMarketData{SourceUpdatedAt: "2026-09-25", LatestCompletedCandle: "2026-09-25", StaleAssets: 1, TotalAssets: 20, HealthyAssets: 19, SignalCycleAligned: true}
	risk := realRiskData{SourceUpdatedAt: "2026-09-25", DecisionCorrelationID: "corr-1", ApprovedGrossTargetLabel: "80.0%", ActiveTargetCount: 8}

	got := buildRealAlertsAudit(infra, recon, nil, exec, nil, market, nil, risk, nil)
	if got.ActiveCritical != 2 {
		t.Fatalf("expected 2 critical derived alerts, got %d", got.ActiveCritical)
	}
	if got.ActiveWarnings < 2 {
		t.Fatalf("expected execution and market warnings, got %d", got.ActiveWarnings)
	}
	if got.AcknowledgementAvailable || got.DurableAlertHistoryAvailable || got.HumanAuditAvailable {
		t.Fatal("Step 24 must not claim persistence/ack/human audit models that do not exist")
	}
	if got.AuditMode != "DERIVED_SYSTEM_EVIDENCE" || len(got.Audit) == 0 {
		t.Fatal("expected derived system evidence timeline")
	}
}

func TestBuildRealAlertsAuditTurnsReadFailuresIntoVisibleWarnings(t *testing.T) {
	infra := realInfrastructure{LastUpdated: "08:00:00 UTC", Postgres: realPostgres{State: "HEALTHY"}, NATS: realNATS{State: "HEALTHY"}}
	got := buildRealAlertsAudit(
		infra,
		realReconciliationData{}, errors.New("recon unavailable"),
		realExecutionData{}, errors.New("execution unavailable"),
		realMarketData{}, errors.New("market unavailable"),
		realRiskData{}, errors.New("risk unavailable"),
	)
	if got.ActiveWarnings != 4 {
		t.Fatalf("expected 4 read-model warnings, got %d", got.ActiveWarnings)
	}
}

func TestMergeSymbolRegistryAlertsDependencyReadFailureIsWarningWhileRoutingDisabled(t *testing.T) {
	base := realAlertsAuditData{}
	registry := SymbolRegistryStatus{
		Status:       "BLOCKED",
		Validated:    false,
		CheckedAt:    "2026-09-26T08:34:26Z",
		OrderRouting: "DISABLED",
		Error:        "current Hyperliquid TESTNET metadata unavailable: temporary network failure",
	}
	got := mergeSymbolRegistryAlerts(base, registry)
	if got.ActiveCritical != 0 {
		t.Fatalf("transient read-only registry dependency must not create a Step32 critical loop, got critical=%d", got.ActiveCritical)
	}
	if got.ActiveWarnings != 1 {
		t.Fatalf("expected exactly one warning, got warnings=%d", got.ActiveWarnings)
	}
	if len(got.Alerts) != 1 || got.Alerts[0].Severity != "WARN" || got.Alerts[0].EventType != "SYMBOL_REGISTRY_BLOCKED" {
		t.Fatalf("unexpected alert: %+v", got.Alerts)
	}
}

func TestMergeSymbolRegistryAlertsArtifactFailureRemainsCritical(t *testing.T) {
	base := realAlertsAuditData{}
	registry := SymbolRegistryStatus{
		Status:       "BLOCKED",
		Validated:    false,
		CheckedAt:    "2026-09-26T08:34:26Z",
		OrderRouting: "DISABLED",
		Error:        "decode symbol registry: invalid character",
	}
	got := mergeSymbolRegistryAlerts(base, registry)
	if got.ActiveCritical != 1 {
		t.Fatalf("registry artifact failure must remain critical, got critical=%d", got.ActiveCritical)
	}
}

func TestMergeSymbolRegistryAlertsUnregisteredAssetRemainsCritical(t *testing.T) {
	base := realAlertsAuditData{}
	registry := SymbolRegistryStatus{
		Status:       "VALIDATED_WITH_WARNINGS",
		Validated:    true,
		CheckedAt:    "2026-09-26T08:34:26Z",
		OrderRouting: "DISABLED",
		Alarms: []SymbolRegistryAlarm{{
			Severity:  "CRITICAL",
			EventType: "SYMBOL_UNREGISTERED_STRATEGY",
			Asset:     "NEWCOINUSDT",
			Title:     "Strategy selected an unregistered symbol",
			Detail:    "must remain non-routable",
		}},
	}
	got := mergeSymbolRegistryAlerts(base, registry)
	if got.ActiveCritical != 1 {
		t.Fatalf("unregistered strategy symbol must remain critical, got critical=%d", got.ActiveCritical)
	}
}

func TestMergeLedgerAlertsIntegrityFailureIsCritical(t *testing.T) {
	base := realAlertsAuditData{}
	ledger := LedgerStatus{Status: "BLOCKED", Validated: false, CheckedAt: "2026-09-26T10:00:00Z", OrderRouting: "DISABLED", Error: "fill_id uniqueness violation: rows=2 distinct=1"}
	got := mergeLedgerAlerts(base, ledger)
	if got.ActiveCritical != 1 || len(got.Alerts) != 1 || got.Alerts[0].EventType != "LEDGER_INTEGRITY_BLOCKED" {
		t.Fatalf("ledger integrity violation must be critical: %+v", got)
	}
}

func TestMergeLedgerAlertsTransientPostgresReadIsWarningWhileRoutingDisabled(t *testing.T) {
	base := realAlertsAuditData{}
	ledger := LedgerStatus{Status: "BLOCKED", Validated: false, CheckedAt: "2026-09-26T10:00:00Z", OrderRouting: "DISABLED", Error: "PostgreSQL query timed out after 2s"}
	got := mergeLedgerAlerts(base, ledger)
	if got.ActiveCritical != 0 || got.ActiveWarnings != 1 || got.Alerts[0].EventType != "LEDGER_READ_UNAVAILABLE" {
		t.Fatalf("transient ledger read should be WARN while routing disabled: %+v", got)
	}
}

func TestMergeDurableAlertHistoryExposesLifecycleAndResolved24h(t *testing.T) {
	dir := t.TempDir()
	store, err := alertstore.Open(dir)
	if err != nil {
		t.Fatal(err)
	}
	now := time.Date(2026, 9, 26, 10, 0, 0, 0, time.UTC)
	alert := alertstore.Alert{ID: "a1", Status: "ACTIVE", Severity: "WARN", Service: "MarketData", EventType: "STALE_DATA", Title: "stale", Detail: "detail"}
	if _, err := store.Apply(now.Add(-time.Hour), []alertstore.Alert{alert}); err != nil {
		t.Fatal(err)
	}
	if _, err := store.Apply(now.Add(-30*time.Minute), nil); err != nil {
		t.Fatal(err)
	}

	got := mergeDurableAlertHistory(realAlertsAuditData{DurableLifecycleEvents: []alertstore.LifecycleEvent{}}, dir, now)
	if !got.WatchdogAvailable || !got.DurableAlertHistoryAvailable || got.Resolved24h != 1 || len(got.DurableLifecycleEvents) != 2 {
		t.Fatalf("unexpected durable history merge: %+v", got)
	}
	if got.AcknowledgementAvailable || got.HumanAuditAvailable {
		t.Fatal("Step 43 must not claim acknowledgement or human-action audit persistence")
	}
}

func TestMergeManualAuditHistoryExposesDurableHumanIntent(t *testing.T) {
	dir := t.TempDir()
	store := manualaudit.New(dir)
	if err := store.Append(manualaudit.Event{
		EventID: "manual-route-1", RecordedAt: "2026-09-26T16:00:00Z", Actor: "operator / OPERATOR",
		Action: "MANUAL_ROUTE_ADMISSION", Target: "portfolio", Result: "REJECTED",
		RequestHash: "sha256:abc", CorrelationID: "manual-route-1", ContractVersion: manualaudit.StoreVersion,
		Submitted: false, Blockers: []string{"PRIVATE_AUTH_DEFERRED"}, Detail: "blocked safely",
	}); err != nil {
		t.Fatal(err)
	}
	got := mergeManualAuditHistory(realAlertsAuditData{Audit: []realAuditRecord{}}, dir)
	if !got.HumanAuditAvailable || got.AuditMode != "DURABLE_MANUAL_INTENT+DURABLE_ALERT_LIFECYCLE+DERIVED_SYSTEM_EVIDENCE" {
		t.Fatalf("manual audit not exposed: %+v", got)
	}
	if len(got.Audit) != 1 || got.Audit[0].ActorType != "HUMAN" || got.Audit[0].Action != "MANUAL_ROUTE_ADMISSION" || got.Audit[0].CorrelationID != "manual-route-1" {
		t.Fatalf("unexpected manual audit projection: %+v", got.Audit)
	}
	if got.AcknowledgementAvailable {
		t.Fatal("Step 46 manual intent must not imply alert acknowledgement persistence")
	}
}

func TestStep46AAcknowledgementBindsExactLifecycleAndDoesNotClearCritical(t *testing.T) {
	ackDir := t.TempDir()
	result := realAlertsAuditData{
		Alerts: []realOperationalAlert{
			{
				ID: "critical-1", Timestamp: "2026-09-26T20:00:00Z", Severity: "CRITICAL", Status: "ACTIVE",
				Service: "Reconciliation", EventType: "RECONCILIATION_BLOCKED", Title: "blocked", Detail: "mismatch",
				AcknowledgementKey: "life-1",
			},
			{
				ID: "warn-1", Timestamp: "2026-09-26T20:00:00Z", Severity: "WARN", Status: "ACTIVE",
				Service: "NATS", EventType: "SOURCE_DEGRADED", Title: "degraded", Detail: "lag",
				AcknowledgementKey: "life-2",
			},
		},
		SourceMode: "REAL",
		SourceNote: "base",
	}
	store := alertack.New(ackDir)
	if err := store.Append(alertack.Event{
		EventID: "ack-1", Actor: "operator / OPERATOR", AlertID: "critical-1", LifecycleEventID: "life-1",
		Severity: "CRITICAL", Service: "Reconciliation", EventType: "RECONCILIATION_BLOCKED",
		Title: "blocked", Comment: "seen", CorrelationID: "ack-1",
	}); err != nil {
		t.Fatal(err)
	}

	got := mergeAlertAcknowledgements(result, ackDir)
	if !got.AcknowledgementAvailable || got.Acknowledged != 1 || got.Alerts[0].Status != "ACKNOWLEDGED" {
		t.Fatalf("acknowledgement not projected: %+v", got)
	}
	if got.ActiveCritical != 1 || got.ActiveWarnings != 1 {
		t.Fatalf("acknowledgement incorrectly suppressed unresolved severity counts: critical=%d warnings=%d", got.ActiveCritical, got.ActiveWarnings)
	}
	foundAudit := false
	for _, row := range got.Audit {
		if row.Action == "ALERT_ACKNOWLEDGE" && row.Target == "critical-1" && row.ActorType == "HUMAN" {
			foundAudit = true
		}
	}
	if !foundAudit {
		t.Fatalf("ack audit evidence missing: %+v", got.Audit)
	}
}

func TestStep46AOldLifecycleAcknowledgementDoesNotApplyToUpdatedAlert(t *testing.T) {
	ackDir := t.TempDir()
	if err := alertack.New(ackDir).Append(alertack.Event{
		EventID: "ack-old", Actor: "operator / OPERATOR", AlertID: "warn-1", LifecycleEventID: "life-old",
		CorrelationID: "ack-old",
	}); err != nil {
		t.Fatal(err)
	}
	result := realAlertsAuditData{
		Alerts: []realOperationalAlert{{ID: "warn-1", Severity: "WARN", Status: "ACTIVE", AcknowledgementKey: "life-new"}},
	}
	got := mergeAlertAcknowledgements(result, ackDir)
	if got.Acknowledged != 0 || got.Alerts[0].Status != "ACTIVE" {
		t.Fatalf("stale lifecycle acknowledgement leaked into updated alert: %+v", got)
	}
}

func TestStep46ACombinedManualIntentAndAlertAckPreserveBothAuditModes(t *testing.T) {
	ackDir := t.TempDir()
	manualDir := t.TempDir()

	if err := alertack.New(ackDir).Append(alertack.Event{
		EventID: "ack-combined-1", Actor: "operator / OPERATOR", AlertID: "critical-1",
		LifecycleEventID: "life-combined-1", Severity: "CRITICAL", Service: "Reconciliation",
		EventType: "RECONCILIATION_BLOCKED", Title: "blocked", CorrelationID: "ack-combined-1",
	}); err != nil {
		t.Fatal(err)
	}
	if err := manualaudit.New(manualDir).Append(manualaudit.Event{
		EventID: "manual-combined-1", RecordedAt: "2026-09-27T07:00:00Z", Actor: "operator / OPERATOR",
		Action: "MANUAL_ROUTE_ADMISSION", Target: "portfolio", Result: "REJECTED",
		RequestHash: "sha256:combined", CorrelationID: "manual-combined-1",
		ContractVersion: manualaudit.StoreVersion, Submitted: false,
		Blockers: []string{"PRIVATE_AUTH_DEFERRED"}, Detail: "blocked safely",
	}); err != nil {
		t.Fatal(err)
	}

	result := realAlertsAuditData{
		Alerts: []realOperationalAlert{{
			ID: "critical-1", Severity: "CRITICAL", Status: "ACTIVE",
			AcknowledgementKey: "life-combined-1",
		}},
		Audit:      []realAuditRecord{},
		SourceNote: "base",
	}
	got := mergeAlertAcknowledgements(result, ackDir)
	got = mergeManualAuditHistory(got, manualDir)

	if !got.AcknowledgementAvailable || !got.HumanAuditAvailable {
		t.Fatalf("combined durable human evidence availability lost: %+v", got)
	}
	wantMode := "DURABLE_MANUAL_INTENT+DURABLE_ALERT_ACK+DURABLE_ALERT_LIFECYCLE+DERIVED_SYSTEM_EVIDENCE"
	if got.AuditMode != wantMode {
		t.Fatalf("combined audit mode lost a durable capability: got=%q want=%q", got.AuditMode, wantMode)
	}
	if !strings.Contains(got.SourceNote, "Step 46A") || !strings.Contains(got.SourceNote, "operator-intent audit store") {
		t.Fatalf("combined source disclosure incomplete: %q", got.SourceNote)
	}

	ackFound, manualFound := false, false
	for _, row := range got.Audit {
		switch row.Action {
		case "ALERT_ACKNOWLEDGE":
			ackFound = row.ActorType == "HUMAN" && row.CorrelationID == "ack-combined-1"
		case "MANUAL_ROUTE_ADMISSION":
			manualFound = row.ActorType == "HUMAN" && row.CorrelationID == "manual-combined-1"
		}
	}
	if !ackFound || !manualFound {
		t.Fatalf("combined HUMAN audit evidence incomplete: %+v", got.Audit)
	}
}
