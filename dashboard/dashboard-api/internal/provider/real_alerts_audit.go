package provider

import (
	"context"
	"fmt"
	"sort"
	"strings"
	"sync"
	"time"

	"control-dashboard-api/internal/alertack"
	"control-dashboard-api/internal/alertstore"
	"control-dashboard-api/internal/manualaudit"
)

// Step 24 intentionally builds a read-only operational projection from already
// verified canonical sources. It does not pretend that the project has a
// durable alert ledger, acknowledgement store, or human-action audit table.
type realAlertsAuditData struct {
	ActiveCritical               int                         `json:"activeCritical"`
	ActiveWarnings               int                         `json:"activeWarnings"`
	Acknowledged                 int                         `json:"acknowledged"`
	Resolved24h                  int                         `json:"resolved24h"`
	Alerts                       []realOperationalAlert      `json:"alerts"`
	Audit                        []realAuditRecord           `json:"audit"`
	SourceMode                   string                      `json:"sourceMode"`
	SourceUpdatedAt              string                      `json:"sourceUpdatedAt"`
	SourceNote                   string                      `json:"sourceNote"`
	AcknowledgementAvailable     bool                        `json:"acknowledgementAvailable"`
	DurableAlertHistoryAvailable bool                        `json:"durableAlertHistoryAvailable"`
	HumanAuditAvailable          bool                        `json:"humanAuditAvailable"`
	AuditMode                    string                      `json:"auditMode"`
	WatchdogAvailable            bool                        `json:"watchdogAvailable"`
	WatchdogState                string                      `json:"watchdogState"`
	WatchdogLastSweepAt          string                      `json:"watchdogLastSweepAt,omitempty"`
	WatchdogLastSuccessAt        string                      `json:"watchdogLastSuccessAt,omitempty"`
	DurableEventCount            int64                       `json:"durableEventCount"`
	DurableLifecycleEvents       []alertstore.LifecycleEvent `json:"durableLifecycleEvents"`
	AcknowledgementEventCount    int                         `json:"acknowledgementEventCount"`
}

type realOperationalAlert struct {
	ID                     string `json:"id"`
	Timestamp              string `json:"timestamp"`
	Severity               string `json:"severity"`
	Status                 string `json:"status"`
	Service                string `json:"service"`
	Asset                  string `json:"asset,omitempty"`
	EventType              string `json:"eventType"`
	Title                  string `json:"title"`
	Detail                 string `json:"detail"`
	CorrelationID          string `json:"correlationId,omitempty"`
	LinkedContext          string `json:"linkedContext,omitempty"`
	AcknowledgementKey     string `json:"acknowledgementKey,omitempty"`
	AcknowledgedAt         string `json:"acknowledgedAt,omitempty"`
	AcknowledgedBy         string `json:"acknowledgedBy,omitempty"`
	AcknowledgementComment string `json:"acknowledgementComment,omitempty"`
}

type realAuditRecord struct {
	ID            string `json:"id"`
	Timestamp     string `json:"timestamp"`
	Actor         string `json:"actor"`
	ActorType     string `json:"actorType"`
	Action        string `json:"action"`
	Target        string `json:"target"`
	Result        string `json:"result"`
	RequestHash   string `json:"requestHash,omitempty"`
	CorrelationID string `json:"correlationId,omitempty"`
	Detail        string `json:"detail"`
}

func (p *Real) alertsAudit(ctx context.Context) realAlertsAuditData {
	// Shell and global readiness can request Alerts & Audit at the same instant.
	// Share that short-lived computation; the watchdog runs in a separate process
	// and therefore remains independently derived/persisted.
	p.alertsCache.mu.Lock()
	if !p.alertsCache.finished.IsZero() && time.Since(p.alertsCache.finished) < dashboardReadCoalesceTTL {
		value := p.alertsCache.value
		p.alertsCache.mu.Unlock()
		return value
	}
	if p.alertsCache.inFlight {
		done := p.alertsCache.done
		p.alertsCache.mu.Unlock()
		select {
		case <-done:
			p.alertsCache.mu.Lock()
			value := p.alertsCache.value
			p.alertsCache.mu.Unlock()
			return value
		case <-ctx.Done():
			return realAlertsAuditData{SourceMode: "REAL", SourceUpdatedAt: time.Now().UTC().Format("15:04:05 UTC"), SourceNote: "Alerts snapshot wait cancelled: " + ctx.Err().Error()}
		}
	}
	p.alertsCache.inFlight = true
	p.alertsCache.done = make(chan struct{})
	done := p.alertsCache.done
	p.alertsCache.mu.Unlock()

	value := p.loadAlertsAudit(ctx)

	p.alertsCache.mu.Lock()
	p.alertsCache.value = value
	p.alertsCache.finished = time.Now()
	p.alertsCache.inFlight = false
	close(done)
	p.alertsCache.mu.Unlock()
	return value
}

func (p *Real) loadAlertsAudit(ctx context.Context) realAlertsAuditData {
	var (
		infra     realInfrastructure
		recon     realReconciliationData
		reconErr  error
		execution realExecutionData
		execErr   error
		market    realMarketData
		marketErr error
		risk      realRiskData
		riskErr   error
		registry  SymbolRegistryStatus
		ledger    LedgerStatus
	)

	// Each source is independent and bounded. Fetch concurrently so one slow
	// optional projection does not consume the entire HTTP read timeout.
	var wg sync.WaitGroup
	wg.Add(7)
	go func() { defer wg.Done(); infra = p.infrastructure(ctx) }()
	go func() { defer wg.Done(); recon, reconErr = p.reconciliation(ctx) }()
	go func() { defer wg.Done(); execution, execErr = p.execution(ctx) }()
	go func() { defer wg.Done(); market, marketErr = p.marketDataResource(ctx) }()
	go func() { defer wg.Done(); risk, riskErr = p.risk(ctx) }()
	go func() { defer wg.Done(); registry = p.SymbolRegistryStatus(ctx) }()
	go func() { defer wg.Done(); ledger = p.LedgerStatus(ctx) }()
	wg.Wait()

	result := buildRealAlertsAudit(infra, recon, reconErr, execution, execErr, market, marketErr, risk, riskErr)
	result = mergeLedgerAlerts(result, ledger)
	result = mergeSymbolRegistryAlerts(result, registry)
	result = mergeDurableAlertHistory(result, p.cfg.AlertStoreDir, time.Now().UTC())
	result = mergeAlertAcknowledgements(result, p.cfg.AlertAckDir)
	return mergeManualAuditHistory(result, p.cfg.ManualAuditDir)
}

func buildRealAlertsAudit(
	infra realInfrastructure,
	recon realReconciliationData,
	reconErr error,
	execution realExecutionData,
	execErr error,
	market realMarketData,
	marketErr error,
	risk realRiskData,
	riskErr error,
) realAlertsAuditData {
	alerts := make([]realOperationalAlert, 0, 24)
	audit := make([]realAuditRecord, 0, 48)

	appendInfraAlerts := func(service, state, detail, linked string) {
		switch state {
		case "CRITICAL":
			alerts = append(alerts, realOperationalAlert{
				ID: "derived-" + strings.ToLower(service) + "-critical", Timestamp: infra.LastUpdated,
				Severity: "CRITICAL", Status: "ACTIVE", Service: service, EventType: "SOURCE_CRITICAL",
				Title: service + " source is critical", Detail: detail, LinkedContext: linked,
			})
		case "WARN":
			alerts = append(alerts, realOperationalAlert{
				ID: "derived-" + strings.ToLower(service) + "-warn", Timestamp: infra.LastUpdated,
				Severity: "WARN", Status: "ACTIVE", Service: service, EventType: "SOURCE_DEGRADED",
				Title: service + " source is degraded", Detail: detail, LinkedContext: linked,
			})
		}
	}
	appendInfraAlerts("PostgreSQL", infra.Postgres.State, infra.Postgres.PersistenceState, "Open Infrastructure")
	appendInfraAlerts("NATS", infra.NATS.State, infra.NATS.AckHealthLabel, "Open Infrastructure")

	if reconErr != nil {
		alerts = append(alerts, realOperationalAlert{
			ID: "derived-reconciliation-read-error", Timestamp: infra.LastUpdated, Severity: "WARN", Status: "ACTIVE",
			Service: "Reconciliation", EventType: "READ_MODEL_UNAVAILABLE", Title: "Reconciliation evidence unavailable",
			Detail: reconErr.Error(), LinkedContext: "Open Reconciliation",
		})
	} else {
		switch recon.Status {
		case "BLOCKED":
			alerts = append(alerts, realOperationalAlert{
				ID: "derived-reconciliation-blocked", Timestamp: recon.LastChecked, Severity: "CRITICAL", Status: "ACTIVE",
				Service: "Reconciliation", EventType: "RECONCILIATION_BLOCKED", Title: "Closed-loop reconciliation is blocked",
				Detail: recon.SourceNote, LinkedContext: "Open Reconciliation",
			})
		case "PENDING":
			alerts = append(alerts, realOperationalAlert{
				ID: "derived-reconciliation-pending", Timestamp: recon.LastChecked, Severity: "WARN", Status: "ACTIVE",
				Service: "Reconciliation", EventType: "RECONCILIATION_PENDING", Title: "Current closed-loop proof is pending",
				Detail: recon.SourceNote, LinkedContext: "Open Reconciliation",
			})
		case "DRIFT":
			alerts = append(alerts, realOperationalAlert{
				ID: "derived-reconciliation-drift", Timestamp: recon.LastChecked, Severity: "WARN", Status: "ACTIVE",
				Service: "Reconciliation", EventType: "RECONCILIATION_DRIFT", Title: "Reconciliation drift observed",
				Detail: recon.SourceNote, LinkedContext: "Open Reconciliation",
			})
		}
		result := "NOOP"
		if recon.Status == "CLEAN" {
			result = "SUCCESS"
		}
		if recon.Status == "BLOCKED" {
			result = "REJECTED"
		}
		audit = append(audit, realAuditRecord{
			ID: "evidence-reconciliation-latest", Timestamp: recon.LastChecked, Actor: "system", ActorType: "SYSTEM",
			Action: "RECONCILIATION_EVIDENCE_OBSERVED", Target: "portfolio", Result: result,
			Detail: fmt.Sprintf("Latest retained reconciliation evidence evaluated as %s. This is source evidence, not a durable dashboard audit row.", recon.Status),
		})
	}

	if execErr != nil {
		alerts = append(alerts, realOperationalAlert{
			ID: "derived-execution-read-error", Timestamp: infra.LastUpdated, Severity: "WARN", Status: "ACTIVE",
			Service: "Execution", EventType: "READ_MODEL_UNAVAILABLE", Title: "Execution evidence unavailable",
			Detail: execErr.Error(), LinkedContext: "Open Execution",
		})
	} else {
		for i, reject := range execution.Rejects {
			if i >= 20 {
				break
			}
			alerts = append(alerts, realOperationalAlert{
				ID: "derived-execution-reject-" + reject.OrderID, Timestamp: execution.SourceUpdatedAt, Severity: "WARN", Status: "ACTIVE",
				Service: "Execution", Asset: reject.Asset, EventType: "ORDER_REJECT", Title: "Durable rejected order " + reject.OrderID,
				Detail: reject.Reason, LinkedContext: "Open Execution",
			})
			audit = append(audit, realAuditRecord{
				ID: "evidence-order-reject-" + reject.OrderID, Timestamp: execution.SourceUpdatedAt, Actor: "system", ActorType: "SYSTEM",
				Action: "ORDER_REJECT_STATE_OBSERVED", Target: reject.Asset + " / order " + reject.OrderID, Result: "REJECTED",
				Detail: reject.Reason,
			})
		}
		for i, fill := range execution.PartialFills {
			if i >= 20 {
				break
			}
			audit = append(audit, realAuditRecord{
				ID: "evidence-fill-" + fill.FillID, Timestamp: fill.Timestamp, Actor: "system", ActorType: "SYSTEM",
				Action: "FILL_PERSISTED", Target: fill.Asset + " / order " + fill.OrderID, Result: "SUCCESS",
				Detail: fmt.Sprintf("Persisted fill %s qty %s at %s; fees %s.", fill.FillID, formatQuantity(fill.Quantity), fill.PriceLabel, fill.FeesLabel),
			})
		}
	}

	if marketErr != nil {
		alerts = append(alerts, realOperationalAlert{
			ID: "derived-market-read-error", Timestamp: infra.LastUpdated, Severity: "WARN", Status: "ACTIVE",
			Service: "MarketData", EventType: "READ_MODEL_UNAVAILABLE", Title: "Canonical market-data diagnostics unavailable",
			Detail: marketErr.Error(), LinkedContext: "Open Market Data",
		})
	} else {
		if !market.SignalCycleAligned {
			alerts = append(alerts, realOperationalAlert{
				ID: "derived-market-cycle-not-aligned", Timestamp: market.SourceUpdatedAt, Severity: "WARN", Status: "ACTIVE",
				Service: "MarketData", EventType: "SIGNAL_CYCLE_NOT_ALIGNED", Title: "Strategy checkpoint is not aligned to canonical market cycle",
				Detail: market.SourceNote, LinkedContext: "Open Market Data",
			})
		}
		if market.StaleAssets > 0 {
			alerts = append(alerts, realOperationalAlert{
				ID: "derived-market-stale-assets", Timestamp: market.SourceUpdatedAt, Severity: "WARN", Status: "ACTIVE",
				Service: "MarketData", EventType: "STALE_DATA", Title: "Stale assets in strategy universe",
				Detail: fmt.Sprintf("%d of %d assets are stale at the canonical frontier.", market.StaleAssets, market.TotalAssets), LinkedContext: "Open Market Data",
			})
		}
		if market.Integrity.InvalidRows > 0 || market.Integrity.DuplicateTimestamps > 0 {
			alerts = append(alerts, realOperationalAlert{
				ID: "derived-market-integrity", Timestamp: market.SourceUpdatedAt, Severity: "WARN", Status: "ACTIVE",
				Service: "MarketData", EventType: "DATA_INTEGRITY", Title: "Canonical market-data integrity issue",
				Detail: fmt.Sprintf("%d invalid row(s), %d duplicate timestamp(s).", market.Integrity.InvalidRows, market.Integrity.DuplicateTimestamps), LinkedContext: "Open Market Data",
			})
		}
		if market.Integrity.MissingCandles > 0 || market.Integrity.Gaps > 0 {
			alerts = append(alerts, realOperationalAlert{
				ID: "derived-market-gaps", Timestamp: market.SourceUpdatedAt, Severity: "WARN", Status: "ACTIVE",
				Service: "MarketData", EventType: "CANDLE_GAPS", Title: "Recent market-data gaps detected",
				Detail: fmt.Sprintf("%d missing candle(s) across %d gap run(s) in the bounded strategy window.", market.Integrity.MissingCandles, market.Integrity.Gaps), LinkedContext: "Open Market Data",
			})
		}
		for i, rejection := range market.CandidateRejections {
			if i >= 10 || rejection.Severity == "INFO" {
				break
			}
			alerts = append(alerts, realOperationalAlert{
				ID: fmt.Sprintf("derived-candidate-%d-%s", i, strings.ToLower(rejection.Asset)), Timestamp: market.SourceUpdatedAt,
				Severity: "WARN", Status: "ACTIVE", Service: "Strategy", Asset: rejection.Asset, EventType: "CANDIDATE_DIAGNOSTIC",
				Title: rejection.Reason, Detail: rejection.Detail, LinkedContext: "Open Market Data",
			})
		}
		audit = append(audit, realAuditRecord{
			ID: "evidence-market-cycle-latest", Timestamp: market.SourceUpdatedAt, Actor: "system", ActorType: "SYSTEM",
			Action: "MARKET_CYCLE_OBSERVED", Target: market.LatestCompletedCandle, Result: "SUCCESS",
			Detail: fmt.Sprintf("Canonical market-data frontier observed with %d/%d healthy strategy-universe assets.", market.HealthyAssets, market.TotalAssets),
		})
	}

	if riskErr != nil {
		alerts = append(alerts, realOperationalAlert{
			ID: "derived-risk-read-error", Timestamp: infra.LastUpdated, Severity: "WARN", Status: "ACTIVE",
			Service: "PortfolioRisk", EventType: "READ_MODEL_UNAVAILABLE", Title: "Risk decision checkpoint unavailable",
			Detail: riskErr.Error(), LinkedContext: "Open Risk",
		})
	} else {
		audit = append(audit, realAuditRecord{
			ID: "evidence-risk-decision-latest", Timestamp: risk.SourceUpdatedAt, Actor: "system", ActorType: "SYSTEM",
			Action: "APPROVED_DECISION_OBSERVED", Target: "portfolio", Result: "SUCCESS", CorrelationID: risk.DecisionCorrelationID,
			Detail: fmt.Sprintf("Approved PortfolioRisk decision observed: %s gross target across %d active target(s).", risk.ApprovedGrossTargetLabel, risk.ActiveTargetCount),
		})
	}

	// Source-health evidence is useful operational context but is not persisted
	// as a product audit ledger. Keep that distinction explicit in the UI note.
	audit = append(audit, realAuditRecord{
		ID: "evidence-source-health-latest", Timestamp: infra.LastUpdated, Actor: "system", ActorType: "SYSTEM",
		Action: "SOURCE_HEALTH_OBSERVED", Target: "PostgreSQL + NATS", Result: sourceHealthResult(infra),
		Detail: fmt.Sprintf("PostgreSQL=%s; NATS=%s. This health observation is computed on request and is not append-only persisted.", infra.Postgres.State, infra.NATS.State),
	})

	critical, warnings := 0, 0
	for _, alert := range alerts {
		if alert.Status != "ACTIVE" {
			continue
		}
		switch alert.Severity {
		case "CRITICAL":
			critical++
		case "WARN":
			warnings++
		}
	}

	sort.SliceStable(alerts, func(i, j int) bool {
		return alertSeverityRank(alerts[i].Severity) > alertSeverityRank(alerts[j].Severity)
	})

	return realAlertsAuditData{
		ActiveCritical:               critical,
		ActiveWarnings:               warnings,
		Acknowledged:                 0,
		Resolved24h:                  0,
		Alerts:                       alerts,
		Audit:                        audit,
		SourceMode:                   "REAL",
		SourceUpdatedAt:              infra.LastUpdated,
		SourceNote:                   "Step 43 derives current operational alerts and system evidence from canonical read-only sources. Durable alert lifecycle history is claimed only when the separate watchdog store has a valid persisted heartbeat; acknowledgement and human-action audit remain intentionally unavailable.",
		AcknowledgementAvailable:     false,
		DurableAlertHistoryAvailable: false,
		HumanAuditAvailable:          false,
		AuditMode:                    "DERIVED_SYSTEM_EVIDENCE",
		WatchdogAvailable:            false,
		WatchdogState:                "NOT_WIRED",
		DurableLifecycleEvents:       []alertstore.LifecycleEvent{},
	}
}

func sourceHealthResult(infra realInfrastructure) string {
	if infra.Postgres.State == "CRITICAL" || infra.NATS.State == "CRITICAL" {
		return "REJECTED"
	}
	if infra.Postgres.State == "WARN" || infra.NATS.State == "WARN" {
		return "NOOP"
	}
	return "SUCCESS"
}

func alertSeverityRank(value string) int {
	switch value {
	case "CRITICAL":
		return 3
	case "WARN":
		return 2
	case "INFO":
		return 1
	default:
		return 0
	}
}

func isSymbolRegistryDependencyReadFailure(detail string) bool {
	detail = strings.TrimSpace(detail)
	return strings.HasPrefix(detail, "canonical market-data ranking unavailable:") ||
		strings.HasPrefix(detail, "current strategy universe unavailable:") ||
		strings.HasPrefix(detail, "current Hyperliquid TESTNET metadata unavailable:")
}

func mergeSymbolRegistryAlerts(result realAlertsAuditData, registry SymbolRegistryStatus) realAlertsAuditData {
	timestamp := registry.CheckedAt
	if strings.TrimSpace(timestamp) == "" {
		timestamp = result.SourceUpdatedAt
	}
	if registry.Status == "BLOCKED" || (!registry.Validated && registry.Status != "MOCK_ONLY") {
		detail := registry.Error
		if detail == "" {
			detail = registry.Note
		}

		// Step 37A is an observability/readiness layer while order routing is still
		// explicitly disabled. A temporary inability to refresh one of its external
		// read-only dependencies must not create a circular precondition where the
		// new registry alert blocks Step 32 -> Step 36 before Step 37A can validate
		// itself. Keep the registry fail-closed (Validated=false, Status=BLOCKED),
		// but expose dependency-read failures as WARN until routing is enabled.
		// Actual registry/artifact/manifest contradictions and the explicit CRITICAL
		// drift/unregistered alarms below remain CRITICAL.
		severity := "CRITICAL"
		if registry.OrderRouting == "DISABLED" && isSymbolRegistryDependencyReadFailure(registry.Error) {
			severity = "WARN"
		}
		result.Alerts = append(result.Alerts, realOperationalAlert{
			ID: "derived-symbol-registry-blocked", Timestamp: timestamp, Severity: severity, Status: "ACTIVE",
			Service: "SymbolRegistry", EventType: "SYMBOL_REGISTRY_BLOCKED", Title: "Multi-exchange symbol registry validation is blocked",
			Detail: detail, LinkedContext: "Open Infrastructure",
		})
	}
	for i, alarm := range registry.Alarms {
		severity := alarm.Severity
		if severity != "CRITICAL" && severity != "WARN" && severity != "INFO" {
			severity = "WARN"
		}
		result.Alerts = append(result.Alerts, realOperationalAlert{
			ID:        fmt.Sprintf("derived-symbol-registry-%03d-%s", i, strings.ToLower(strings.ReplaceAll(alarm.EventType, "_", "-"))),
			Timestamp: timestamp, Severity: severity, Status: "ACTIVE", Service: "SymbolRegistry", Asset: alarm.Asset,
			EventType: alarm.EventType, Title: alarm.Title, Detail: alarm.Detail, LinkedContext: "Open Infrastructure",
		})
	}
	if registry.RegistryVersion != "" {
		result.Audit = append(result.Audit, realAuditRecord{
			ID: "evidence-symbol-registry-latest", Timestamp: timestamp, Actor: "system", ActorType: "SYSTEM",
			Action: "SYMBOL_REGISTRY_OBSERVED", Target: registry.RegistryVersion,
			Result: func() string {
				if registry.Validated {
					return "SUCCESS"
				}
				return "REJECTED"
			}(),
			RequestHash: registry.RegistryArtifactSHA256,
			Detail:      fmt.Sprintf("Multi-exchange symbol registry observed: entries=%d, current ranking=%d/%d registered, current strategy routable=%d/%d, status=%s. This is derived evidence, not a durable audit ledger row.", registry.RegistryEntryCount, registry.CurrentRankingRegisteredCount, registry.CurrentRankingCount, registry.CurrentStrategyRoutableCount, registry.CurrentStrategyCount, registry.Status),
		})
	}

	critical, warnings := 0, 0
	for _, alert := range result.Alerts {
		if alert.Status == "RESOLVED" {
			continue
		}
		switch alert.Severity {
		case "CRITICAL":
			critical++
		case "WARN":
			warnings++
		}
	}
	result.ActiveCritical = critical
	result.ActiveWarnings = warnings
	sort.SliceStable(result.Alerts, func(i, j int) bool {
		return alertSeverityRank(result.Alerts[i].Severity) > alertSeverityRank(result.Alerts[j].Severity)
	})
	if registry.RegistryVersion != "" {
		result.SourceNote += " Step 37A also derives live symbol-registry drift/coverage alerts from the versioned registry, current canonical market ranking/strategy universe and public Hyperliquid TESTNET metadata; these alerts are still not append-only persisted until the dedicated alert store is implemented."
	}
	return result
}

func isLedgerDependencyReadFailure(detail string) bool {
	detail = strings.ToLower(strings.TrimSpace(detail))
	return strings.Contains(detail, "postgresql query timed out") ||
		strings.Contains(detail, "connection") ||
		strings.Contains(detail, "dial") ||
		strings.Contains(detail, "psql") ||
		strings.Contains(detail, "server closed")
}

func mergeLedgerAlerts(result realAlertsAuditData, ledger LedgerStatus) realAlertsAuditData {
	timestamp := ledger.CheckedAt
	if strings.TrimSpace(timestamp) == "" {
		timestamp = result.SourceUpdatedAt
	}
	if ledger.Status == "BLOCKED" || !ledger.Validated {
		severity := "CRITICAL"
		eventType := "LEDGER_INTEGRITY_BLOCKED"
		title := "Append-only ledger integrity validation is blocked"
		if ledger.OrderRouting == "DISABLED" && isLedgerDependencyReadFailure(ledger.Error) {
			severity = "WARN"
			eventType = "LEDGER_READ_UNAVAILABLE"
			title = "Append-only ledger source is temporarily unavailable"
		}
		detail := strings.TrimSpace(ledger.Error)
		if detail == "" {
			detail = ledger.Note
		}
		result.Alerts = append(result.Alerts, realOperationalAlert{
			ID: "derived-ledger-status", Timestamp: timestamp, Severity: severity, Status: "ACTIVE",
			Service: "Ledger", EventType: eventType, Title: title, Detail: detail, LinkedContext: "Open Execution",
		})
	} else {
		result.Audit = append(result.Audit, realAuditRecord{
			ID: "evidence-ledger-latest", Timestamp: timestamp, Actor: "system", ActorType: "SYSTEM",
			Action: "LEDGER_INTEGRITY_OBSERVED", Target: ledger.SourceTable, Result: "SUCCESS",
			RequestHash: ledger.RecentWindowFingerprint,
			Detail:      fmt.Sprintf("Append-only fill ledger validated: rows=%d distinct_fill_ids=%d invalid_rows=%d recent_window=%d. This is source evidence; Step 43 persists alert lifecycle transitions, not accounting mutations.", ledger.TotalFillRows, ledger.DistinctFillIDs, ledger.InvalidFillRows, ledger.RecentWindowCount),
		})
	}
	recountAlerts(&result)
	return result
}

func mergeDurableAlertHistory(result realAlertsAuditData, dir string, now time.Time) realAlertsAuditData {
	snapshot := alertstore.ReadSnapshot(dir, 250)
	if !snapshot.Available {
		result.WatchdogAvailable = false
		result.WatchdogState = "UNAVAILABLE"
		result.DurableAlertHistoryAvailable = false
		result.DurableLifecycleEvents = []alertstore.LifecycleEvent{}
		if strings.TrimSpace(dir) != "" {
			result.Alerts = append(result.Alerts, realOperationalAlert{
				ID: "derived-alert-watchdog-unavailable", Timestamp: result.SourceUpdatedAt, Severity: "WARN", Status: "ACTIVE",
				Service: "AlertWatchdog", EventType: "WATCHDOG_UNAVAILABLE", Title: "Durable alert watchdog store is unavailable",
				Detail: snapshot.Error, LinkedContext: "Open Alerts & Audit",
			})
		}
		recountAlerts(&result)
		return result
	}

	result.WatchdogAvailable = true
	result.WatchdogState = "HEALTHY"
	result.WatchdogLastSweepAt = snapshot.Heartbeat.LastSweepAt
	result.WatchdogLastSuccessAt = snapshot.Heartbeat.LastSuccessAt
	result.DurableEventCount = snapshot.Heartbeat.EventCount
	result.DurableLifecycleEvents = snapshot.Events
	result.DurableAlertHistoryAvailable = true
	result.AuditMode = "DURABLE_ALERT_LIFECYCLE+DERIVED_SYSTEM_EVIDENCE"

	resolved24h := 0
	cutoff := now.UTC().Add(-24 * time.Hour)
	for _, event := range snapshot.Events {
		if event.Transition != "RESOLVED" {
			continue
		}
		when, err := time.Parse(time.RFC3339Nano, event.RecordedAt)
		if err == nil && !when.Before(cutoff) {
			resolved24h++
		}
	}
	result.Resolved24h = resolved24h

	// Acknowledgement is bound to the latest durable OPENED/UPDATED lifecycle
	// event, not merely the alert ID. If the alert materially changes, the
	// watchdog emits a new lifecycle event and the old acknowledgement no longer
	// applies. Refuse to expose an acknowledgement key until the watchdog has
	// persisted the exact current alert material.
	for i := range result.Alerts {
		current := &result.Alerts[i]
		active, ok := snapshot.Active[current.ID]
		if !ok || !sameDurableAlertMaterial(*current, active.Alert) {
			continue
		}
		current.AcknowledgementKey = active.LifecycleEventID
	}

	if snapshot.Heartbeat.LastError != "" {
		result.WatchdogState = "DEGRADED"
		result.Alerts = append(result.Alerts, realOperationalAlert{
			ID: "derived-alert-watchdog-last-error", Timestamp: snapshot.Heartbeat.LastSweepAt, Severity: "WARN", Status: "ACTIVE",
			Service: "AlertWatchdog", EventType: "WATCHDOG_SWEEP_ERROR", Title: "Durable alert watchdog last sweep failed",
			Detail: snapshot.Heartbeat.LastError, LinkedContext: "Open Alerts & Audit",
		})
	}
	if last, err := time.Parse(time.RFC3339, snapshot.Heartbeat.LastSuccessAt); err != nil || now.UTC().Sub(last) > 90*time.Second {
		result.WatchdogState = "STALE"
		result.Alerts = append(result.Alerts, realOperationalAlert{
			ID: "derived-alert-watchdog-stale", Timestamp: snapshot.Heartbeat.LastSweepAt, Severity: "WARN", Status: "ACTIVE",
			Service: "AlertWatchdog", EventType: "WATCHDOG_STALE", Title: "Durable alert watchdog heartbeat is stale",
			Detail: "No successful watchdog sweep has been persisted within the last 90 seconds. Trading remains independent and routing is still disabled.", LinkedContext: "Open Alerts & Audit",
		})
	}

	result.SourceNote = "Step 43 keeps current operational alerts derived read-only from canonical sources while a separate watchdog process persists OPENED/UPDATED/RESOLVED alert lifecycle transitions to its own append-only observability store. Human acknowledgement is a separate Step 46A store and never mutates watchdog lifecycle or trading state."
	recountAlerts(&result)
	return result
}

func sameDurableAlertMaterial(current realOperationalAlert, durable alertstore.Alert) bool {
	return current.Severity == durable.Severity &&
		current.Service == durable.Service &&
		current.Asset == durable.Asset &&
		current.EventType == durable.EventType &&
		current.Title == durable.Title &&
		current.Detail == durable.Detail &&
		current.CorrelationID == durable.CorrelationID &&
		current.LinkedContext == durable.LinkedContext
}

func mergeAlertAcknowledgements(result realAlertsAuditData, dir string) realAlertsAuditData {
	snapshot := alertack.ReadSnapshot(dir, 100)
	if !snapshot.Available {
		result.AcknowledgementAvailable = false
		result.Acknowledged = 0
		if strings.TrimSpace(dir) != "" {
			result.SourceNote += " Step 46A alert acknowledgement store is unavailable: " + snapshot.Error
		}
		return result
	}

	result.AcknowledgementAvailable = true
	result.AcknowledgementEventCount = snapshot.Count
	acknowledged := 0
	for i := range result.Alerts {
		alert := &result.Alerts[i]
		if alert.Status == "RESOLVED" || strings.TrimSpace(alert.AcknowledgementKey) == "" {
			continue
		}
		event, ok := snapshot.LatestByLifecycle[alert.AcknowledgementKey]
		if !ok || event.AlertID != alert.ID {
			continue
		}
		alert.Status = "ACKNOWLEDGED"
		alert.AcknowledgedAt = event.RecordedAt
		alert.AcknowledgedBy = event.Actor
		alert.AcknowledgementComment = event.Comment
		acknowledged++
	}
	result.Acknowledged = acknowledged

	for _, event := range snapshot.Events {
		detail := "Alert acknowledged without changing resolution state."
		if strings.TrimSpace(event.Comment) != "" {
			detail += " Comment: " + event.Comment
		}
		result.Audit = append(result.Audit, realAuditRecord{
			ID: event.EventID, Timestamp: event.RecordedAt, Actor: event.Actor, ActorType: "HUMAN",
			Action: "ALERT_ACKNOWLEDGE", Target: event.AlertID, Result: "SUCCESS",
			CorrelationID: event.CorrelationID, Detail: detail,
		})
	}
	result.AuditMode = "DURABLE_ALERT_ACK+DURABLE_ALERT_LIFECYCLE+DERIVED_SYSTEM_EVIDENCE"
	result.SourceNote += " Step 46A persists human acknowledgements in a separate append-only store, bound to the exact durable alert lifecycle event. ACK never resolves an alert, suppresses critical readiness, or writes trading state."
	recountAlerts(&result)
	sort.SliceStable(result.Audit, func(i, j int) bool { return result.Audit[i].Timestamp > result.Audit[j].Timestamp })
	return result
}

func mergeManualAuditHistory(result realAlertsAuditData, dir string) realAlertsAuditData {
	snapshot := manualaudit.ReadRecent(dir, 50)
	if !snapshot.Available {
		result.HumanAuditAvailable = false
		if strings.TrimSpace(dir) != "" {
			result.SourceNote += " Step 46 durable manual operator-intent audit is unavailable: " + snapshot.Error
		}
		return result
	}

	result.HumanAuditAvailable = true
	// Preserve the Step 46A acknowledgement capability when the Step 46
	// manual-intent audit store is merged afterwards. AuditMode describes all
	// durable human evidence sources exposed by this projection; a later merge
	// must not erase an earlier capability.
	if result.AcknowledgementAvailable {
		result.AuditMode = "DURABLE_MANUAL_INTENT+DURABLE_ALERT_ACK+DURABLE_ALERT_LIFECYCLE+DERIVED_SYSTEM_EVIDENCE"
	} else {
		result.AuditMode = "DURABLE_MANUAL_INTENT+DURABLE_ALERT_LIFECYCLE+DERIVED_SYSTEM_EVIDENCE"
	}
	for _, event := range snapshot.Events {
		result.Audit = append(result.Audit, realAuditRecord{
			ID: event.EventID, Timestamp: event.RecordedAt, Actor: event.Actor, ActorType: "HUMAN",
			Action: event.Action, Target: event.Target, Result: event.Result,
			RequestHash: event.RequestHash, CorrelationID: event.CorrelationID, Detail: event.Detail,
		})
	}
	sort.SliceStable(result.Audit, func(i, j int) bool { return result.Audit[i].Timestamp > result.Audit[j].Timestamp })
	result.SourceNote += " Step 46 keeps confirmed manual route-admission attempts in a separate append-only operator-intent audit store. Manual audit records never write trading PostgreSQL/NATS or imply that an order was submitted."
	return result
}

func recountAlerts(result *realAlertsAuditData) {
	critical, warnings := 0, 0
	for _, alert := range result.Alerts {
		if alert.Status == "RESOLVED" {
			continue
		}
		switch alert.Severity {
		case "CRITICAL":
			critical++
		case "WARN":
			warnings++
		}
	}
	result.ActiveCritical = critical
	result.ActiveWarnings = warnings
	sort.SliceStable(result.Alerts, func(i, j int) bool {
		return alertSeverityRank(result.Alerts[i].Severity) > alertSeverityRank(result.Alerts[j].Severity)
	})
}
