package provider

import (
	"context"
	"fmt"
	"strings"
	"sync"
	"time"
)

type realShellStatus struct {
	Mode               string   `json:"mode"`
	Exchange           string   `json:"exchange"`
	ExchangeConnected  bool     `json:"exchangeConnected"`
	ExchangeState      string   `json:"exchangeState"`
	ExchangeDetail     string   `json:"exchangeDetail"`
	DataHealthy        bool     `json:"dataHealthy"`
	DataState          string   `json:"dataState"`
	DataDetail         string   `json:"dataDetail"`
	TradingEnabled     bool     `json:"tradingEnabled"`
	TradingState       string   `json:"tradingState"`
	TradingDetail      string   `json:"tradingDetail"`
	Reconciliation     string   `json:"reconciliation"`
	Readiness          string   `json:"readiness"`
	ReadinessDetail    string   `json:"readinessDetail"`
	AlertCount         int      `json:"alertCount"`
	CriticalAlertCount int      `json:"criticalAlertCount"`
	WarningAlertCount  int      `json:"warningAlertCount"`
	UTCLabel           string   `json:"utcLabel"`
	Blockers           []string `json:"blockers"`
	Warnings           []string `json:"warnings"`
	SourceMode         string   `json:"sourceMode"`
}

func (p *Real) shellStatus(ctx context.Context) realShellStatus {
	var (
		infra     realInfrastructure
		recon     realReconciliationData
		reconErr  error
		market    realMarketData
		marketErr error
		alerts    realAlertsAuditData
	)

	var wg sync.WaitGroup
	wg.Add(4)
	go func() { defer wg.Done(); infra = p.infrastructure(ctx) }()
	go func() { defer wg.Done(); recon, reconErr = p.reconciliation(ctx) }()
	go func() { defer wg.Done(); market, marketErr = p.marketDataResource(ctx) }()
	go func() { defer wg.Done(); alerts = p.alertsAudit(ctx) }()
	wg.Wait()

	return buildRealShellStatus(p.runtimeMode(), infra, recon, reconErr, market, marketErr, alerts, time.Now().UTC())
}

func buildRealShellStatus(
	mode string,
	infra realInfrastructure,
	recon realReconciliationData,
	reconErr error,
	market realMarketData,
	marketErr error,
	alerts realAlertsAuditData,
	now time.Time,
) realShellStatus {
	blockers := make([]string, 0, 8)
	warnings := make([]string, 0, 12)

	if infra.Postgres.State == "CRITICAL" {
		blockers = append(blockers, "PostgreSQL is unavailable or authenticated reads are failing")
	} else if infra.Postgres.State == "WARN" {
		warnings = append(warnings, "PostgreSQL source is degraded")
	}
	if infra.NATS.State == "CRITICAL" {
		blockers = append(blockers, "NATS / JetStream is unavailable")
	} else if infra.NATS.State == "WARN" {
		warnings = append(warnings, "NATS is reachable but diagnostics are degraded")
	}
	if len(infra.Services) == 0 {
		blockers = append(blockers, "ExecutionState durable runtime snapshot is not present")
	}

	dataState := "HEALTHY"
	dataDetail := "Canonical SQLite strategy window is readable and aligned to the durable Strategy checkpoint."
	dataHealthy := true
	switch {
	case marketErr != nil:
		dataState = "BLOCKED"
		dataHealthy = false
		dataDetail = "Canonical market-data diagnostics are unavailable: " + marketErr.Error()
		blockers = append(blockers, "Canonical market-data diagnostics are unavailable")
	case market.TotalAssets == 0:
		dataState = "BLOCKED"
		dataHealthy = false
		dataDetail = "Canonical market-data read succeeded but the strategy universe is empty."
		blockers = append(blockers, "Strategy market-data universe is empty")
	case !market.SignalCycleAligned:
		dataState = "DEGRADED"
		dataHealthy = false
		dataDetail = "SQLite is readable, but the durable Strategy checkpoint is not aligned to the canonical market cycle."
		warnings = append(warnings, "Strategy checkpoint is not aligned to the canonical market cycle")
	case market.StaleAssets > 0 || market.Integrity.InvalidRows > 0 || market.Integrity.DuplicateTimestamps > 0 || market.Integrity.MissingCandles > 0 || market.Integrity.Gaps > 0:
		dataState = "DEGRADED"
		dataHealthy = false
		dataDetail = fmt.Sprintf("Market-data diagnostics: stale=%d invalid=%d duplicates=%d missing=%d gaps=%d.", market.StaleAssets, market.Integrity.InvalidRows, market.Integrity.DuplicateTimestamps, market.Integrity.MissingCandles, market.Integrity.Gaps)
		warnings = append(warnings, "Canonical market-data diagnostics report stale/integrity/gap findings")
	}

	reconciliation := "PENDING"
	if reconErr != nil {
		warnings = append(warnings, "Reconciliation read model is unavailable")
	} else if strings.TrimSpace(recon.Status) != "" {
		reconciliation = recon.Status
		switch recon.Status {
		case "BLOCKED":
			blockers = append(blockers, "Current reconciliation evidence is BLOCKED")
		case "PENDING":
			warnings = append(warnings, "Reconciliation is pending current exchange evidence")
		case "DRIFT":
			warnings = append(warnings, "Reconciliation reports drift")
		}
	}

	exchangeState := "FOUNDATION"
	exchangeDetail := fmt.Sprintf("%s TESTNET public metadata connectivity is observed separately by Step 34; execution-gateway/private connectivity remains unverified and fail-closed.", infra.Exchange.Venue)
	if reconErr == nil && recon.ComparisonAvailable {
		if recon.EvidenceFresh {
			exchangeState = "EVIDENCE FRESH"
			exchangeDetail = "A retained exchange snapshot is current relative to the latest runtime snapshot. This proves fresh exchange evidence, not live gateway connectivity."
		} else {
			exchangeState = "EVIDENCE STALE"
			exchangeDetail = "A retained exchange snapshot exists but predates the current runtime snapshot."
			warnings = append(warnings, "Retained exchange evidence is stale")
		}
	} else if reconErr == nil {
		warnings = append(warnings, "No current exchange snapshot is available for closed-loop proof")
	}

	// Trading enable/disable is deliberately not inferred from process existence.
	// The project still lacks a common service-ready/paused control contract.
	tradingState := "UNVERIFIED"
	tradingDetail := "Pipeline read models are available, but no canonical global trading-enabled/paused state is exposed yet."
	if len(blockers) > 0 {
		tradingState = "BLOCKED"
		tradingDetail = "One or more verified hard dependencies are currently blocking a safe readiness verdict."
	}

	// READY is intentionally fail-closed. The master dashboard contract requires
	// exchange connectivity, service readiness and host clock sync, none of which
	// are yet independently verified by the dashboard backend.
	readiness := "DEGRADED"
	readinessDetail := "Verified read models are available, but READY is not asserted until exchange connectivity, common service liveness/trading state and host clock sync are independently observable."
	if len(blockers) > 0 {
		readiness = "PAUSED"
		readinessDetail = "Hard blocker: " + blockers[0]
	}

	if !infra.Exchange.Connected {
		warnings = append(warnings, "Exchange public connectivity is not checked yet")
	}
	warnings = append(warnings,
		"Common trading-service liveness/control state is unverified",
		"Host clock synchronization is unverified",
	)

	return realShellStatus{
		Mode:               mode,
		Exchange:           infra.Exchange.Venue,
		ExchangeConnected:  false,
		ExchangeState:      exchangeState,
		ExchangeDetail:     exchangeDetail,
		DataHealthy:        dataHealthy,
		DataState:          dataState,
		DataDetail:         dataDetail,
		TradingEnabled:     false,
		TradingState:       tradingState,
		TradingDetail:      tradingDetail,
		Reconciliation:     reconciliation,
		Readiness:          readiness,
		ReadinessDetail:    readinessDetail,
		AlertCount:         alerts.ActiveCritical + alerts.ActiveWarnings,
		CriticalAlertCount: alerts.ActiveCritical,
		WarningAlertCount:  alerts.ActiveWarnings,
		UTCLabel:           "UTC " + now.Format("2006-01-02 15:04:05"),
		Blockers:           blockers,
		Warnings:           warnings,
		SourceMode:         "REAL",
	}
}
