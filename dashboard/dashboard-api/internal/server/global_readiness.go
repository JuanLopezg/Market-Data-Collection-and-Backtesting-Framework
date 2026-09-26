package server

import (
	"context"
	"fmt"
	"net/http"
	"sort"
	"strings"
	"sync"
	"time"

	"control-dashboard-api/internal/provider"
)

const globalReadinessContractVersion = "step44-v1"

type globalReadinessRequirement struct {
	ID          string   `json:"id"`
	Label       string   `json:"label"`
	State       string   `json:"state"`
	RequiredNow bool     `json:"requiredNow"`
	RequiredFor []string `json:"requiredFor"`
	Evidence    string   `json:"evidence"`
	Detail      string   `json:"detail"`
	Retryable   bool     `json:"retryable"`
}

type globalReadinessResponse struct {
	ContractVersion         string                       `json:"contractVersion"`
	Status                  string                       `json:"status"`
	ContractComplete        bool                         `json:"contractComplete"`
	CurrentPhase            string                       `json:"currentPhase"`
	SafeToContinueDashboard bool                         `json:"safeToContinueDashboard"`
	PrivateTestnetReady     bool                         `json:"privateTestnetReady"`
	TradingReady            bool                         `json:"tradingReady"`
	LiveReady               bool                         `json:"liveReady"`
	OrderRouting            string                       `json:"orderRouting"`
	PrivateAuth             string                       `json:"privateAuth"`
	ManualRouting           string                       `json:"manualRouting"`
	BlockingCount           int                          `json:"blockingCount"`
	WarningCount            int                          `json:"warningCount"`
	DeferredCount           int                          `json:"deferredCount"`
	RetryableBlockingCount  int                          `json:"retryableBlockingCount"`
	Blockers                []string                     `json:"blockers"`
	Warnings                []string                     `json:"warnings"`
	Deferred                []string                     `json:"deferred"`
	Requirements            []globalReadinessRequirement `json:"requirements"`
	CheckedAt               string                       `json:"checkedAt"`
	NextSafeStep            string                       `json:"nextSafeStep"`
	FutureReplayBoundary    string                       `json:"futureReplayBoundary"`
	Note                    string                       `json:"note"`
}

type readinessShellSnapshot struct {
	Readiness          string   `json:"readiness"`
	ReadinessDetail    string   `json:"readinessDetail"`
	TradingEnabled     bool     `json:"tradingEnabled"`
	TradingState       string   `json:"tradingState"`
	ExchangeConnected  bool     `json:"exchangeConnected"`
	Reconciliation     string   `json:"reconciliation"`
	DataState          string   `json:"dataState"`
	DataDetail         string   `json:"dataDetail"`
	CriticalAlertCount int      `json:"criticalAlertCount"`
	Blockers           []string `json:"blockers"`
}

type readinessInfraSnapshot struct {
	Postgres struct {
		State            string `json:"state"`
		PersistenceState string `json:"persistenceState"`
	} `json:"postgres"`
	NATS struct {
		State          string `json:"state"`
		AckHealthLabel string `json:"ackHealthLabel"`
	} `json:"nats"`
	Services []struct {
		Service string `json:"service"`
	} `json:"services"`
}

type readinessPipelineSnapshot struct {
	Proof *struct {
		Status     string `json:"status"`
		Completion string `json:"completion"`
	} `json:"proof"`
}

type readinessOperationalAlert struct {
	Severity  string `json:"severity"`
	Status    string `json:"status"`
	Service   string `json:"service"`
	EventType string `json:"eventType"`
	Detail    string `json:"detail"`
}

type readinessAlertsSnapshot struct {
	ActiveCritical               int                         `json:"activeCritical"`
	ActiveWarnings               int                         `json:"activeWarnings"`
	Alerts                       []readinessOperationalAlert `json:"alerts"`
	WatchdogAvailable            bool                        `json:"watchdogAvailable"`
	WatchdogState                string                      `json:"watchdogState"`
	WatchdogLastSuccessAt        string                      `json:"watchdogLastSuccessAt"`
	DurableAlertHistoryAvailable bool                        `json:"durableAlertHistoryAvailable"`
}

type readinessManualSnapshot struct {
	ContractVersion      string `json:"contractVersion"`
	RoutingContractReady bool   `json:"routingContractReady"`
	HumanAuditAvailable  bool   `json:"humanAuditAvailable"`
	RouteEnabled         bool   `json:"routeEnabled"`
}

type globalReadinessInputs struct {
	ProviderHealth provider.Health
	Shell          readinessShellSnapshot
	ShellErr       error
	Infra          readinessInfraSnapshot
	InfraErr       error
	Pipeline       readinessPipelineSnapshot
	PipelineErr    error
	Alerts         readinessAlertsSnapshot
	AlertsErr      error
	Manual         readinessManualSnapshot
	ManualErr      error
	Foundation     provider.VenueFoundation
	FoundationOK   bool
	VenuePublic    provider.VenuePublicStatus
	VenuePublicOK  bool
	VenueRules     provider.VenueTradingRulesStatus
	VenueRulesOK   bool
	Registry       provider.SymbolRegistryStatus
	RegistryOK     bool
	Ledger         provider.LedgerStatus
	LedgerOK       bool
	DemoAuth       bool
	CookieSecure   bool
	Now            time.Time
}

func (s *Server) globalReadiness(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := context.WithTimeout(r.Context(), s.cfg.ResourceTimeout)
	defer cancel()

	inputs := globalReadinessInputs{
		ProviderHealth: s.provider.Health(ctx),
		DemoAuth:       s.cfg.DemoAuthAllowed,
		CookieSecure:   s.cfg.CookieSecure,
		Now:            time.Now().UTC(),
	}

	var wg sync.WaitGroup
	wg.Add(5)
	go func() {
		defer wg.Done()
		inputs.ShellErr = s.readSafetyResource(ctx, provider.ResourceShellStatus, &inputs.Shell)
	}()
	go func() {
		defer wg.Done()
		inputs.InfraErr = s.readSafetyResource(ctx, provider.ResourceInfrastructure, &inputs.Infra)
	}()
	go func() {
		defer wg.Done()
		inputs.PipelineErr = s.readSafetyResource(ctx, provider.ResourcePipeline, &inputs.Pipeline)
	}()
	go func() {
		defer wg.Done()
		inputs.AlertsErr = s.readSafetyResource(ctx, provider.ResourceAlertsAudit, &inputs.Alerts)
	}()
	go func() {
		defer wg.Done()
		inputs.ManualErr = s.readSafetyResource(ctx, provider.ResourceManualControl, &inputs.Manual)
	}()

	if reader, ok := s.provider.(interface {
		VenueFoundation(context.Context) provider.VenueFoundation
	}); ok {
		inputs.FoundationOK = true
		inputs.Foundation = reader.VenueFoundation(ctx)
	}

	if reader, ok := s.provider.(interface {
		VenuePublicStatus(context.Context) provider.VenuePublicStatus
	}); ok {
		wg.Add(1)
		go func() {
			defer wg.Done()
			inputs.VenuePublic = reader.VenuePublicStatus(ctx)
			inputs.VenuePublicOK = true
		}()
	}
	if reader, ok := s.provider.(interface {
		VenueTradingRulesStatus(context.Context) provider.VenueTradingRulesStatus
	}); ok {
		wg.Add(1)
		go func() {
			defer wg.Done()
			inputs.VenueRules = reader.VenueTradingRulesStatus(ctx)
			inputs.VenueRulesOK = true
		}()
	}
	if reader, ok := s.provider.(interface {
		SymbolRegistryStatus(context.Context) provider.SymbolRegistryStatus
	}); ok {
		wg.Add(1)
		go func() { defer wg.Done(); inputs.Registry = reader.SymbolRegistryStatus(ctx); inputs.RegistryOK = true }()
	}
	if reader, ok := s.provider.(interface {
		LedgerStatus(context.Context) provider.LedgerStatus
	}); ok {
		wg.Add(1)
		go func() { defer wg.Done(); inputs.Ledger = reader.LedgerStatus(ctx); inputs.LedgerOK = true }()
	}
	wg.Wait()

	s.writeJSON(w, http.StatusOK, buildGlobalReadiness(inputs))
}

func buildGlobalReadiness(in globalReadinessInputs) globalReadinessResponse {
	now := in.Now.UTC()
	if now.IsZero() {
		now = time.Now().UTC()
	}

	requirements := make([]globalReadinessRequirement, 0, 20)
	add := func(id, label, state string, requiredNow bool, requiredFor []string, evidence, detail string) {
		requirements = append(requirements, globalReadinessRequirement{
			ID: id, Label: label, State: state, RequiredNow: requiredNow,
			RequiredFor: requiredFor, Evidence: evidence, Detail: detail,
		})
	}

	if strings.EqualFold(in.ProviderHealth.Mode, "real") {
		add("provider-real", "Real read-only provider", "PASS", true, []string{"DASHBOARD", "PRIVATE_TESTNET", "LIVE"}, "/api/provider-status", "Real provider is active; Step 44 never accepts an implicit mock fallback for this environment.")
	} else {
		add("provider-real", "Real read-only provider", "BLOCKED", true, []string{"DASHBOARD", "PRIVATE_TESTNET", "LIVE"}, "/api/provider-status", "Current provider mode is "+in.ProviderHealth.Mode+"; this contract requires real source evidence.")
	}

	if in.InfraErr != nil {
		add("postgres", "PostgreSQL read path", "BLOCKED", true, []string{"DASHBOARD", "PRIVATE_TESTNET", "LIVE", "REPLAY_MOCK"}, "/api/infrastructure", "Infrastructure evidence unavailable: "+in.InfraErr.Error())
		add("nats", "NATS / JetStream read path", "BLOCKED", true, []string{"DASHBOARD", "PRIVATE_TESTNET", "LIVE", "REPLAY_MOCK"}, "/api/infrastructure", "Infrastructure evidence unavailable: "+in.InfraErr.Error())
	} else {
		addInfraRequirement := func(id, label, state, sourceDetail string) {
			detail := strings.TrimSpace(sourceDetail)
			switch state {
			case "HEALTHY":
				if detail == "" {
					detail = state
				}
				add(id, label, "PASS", true, []string{"DASHBOARD", "PRIVATE_TESTNET", "LIVE", "REPLAY_MOCK"}, "/api/infrastructure", detail)
			case "WARN":
				if detail == "" {
					detail = "Read path is reachable but degraded."
				}
				add(id, label, "WARN", true, []string{"DASHBOARD", "PRIVATE_TESTNET", "LIVE", "REPLAY_MOCK"}, "/api/infrastructure", detail)
			default:
				if detail == "" {
					detail = "Read path state is " + state + "."
				}
				add(id, label, "BLOCKED", true, []string{"DASHBOARD", "PRIVATE_TESTNET", "LIVE", "REPLAY_MOCK"}, "/api/infrastructure", detail)
			}
		}
		addInfraRequirement("postgres", "PostgreSQL read path", in.Infra.Postgres.State, in.Infra.Postgres.PersistenceState)
		addInfraRequirement("nats", "NATS / JetStream read path", in.Infra.NATS.State, in.Infra.NATS.AckHealthLabel)
	}

	if in.ShellErr != nil {
		add("canonical-market-data", "Canonical market-data / strategy universe", "BLOCKED", true, []string{"DASHBOARD", "PRIVATE_TESTNET", "LIVE", "REPLAY_MOCK"}, "/api/shell-status", "Shell evidence unavailable: "+in.ShellErr.Error())
		add("reconciliation", "Reconciliation evidence", "WARN", false, []string{"PRIVATE_TESTNET", "LIVE", "REPLAY_MOCK"}, "/api/reconciliation", "Shell reconciliation evidence is unavailable; private trading remains fail-closed.")
	} else {
		switch in.Shell.DataState {
		case "HEALTHY":
			add("canonical-market-data", "Canonical market-data / strategy universe", "PASS", true, []string{"DASHBOARD", "PRIVATE_TESTNET", "LIVE", "REPLAY_MOCK"}, "/api/shell-status", in.Shell.DataDetail)
		case "DEGRADED":
			add("canonical-market-data", "Canonical market-data / strategy universe", "WARN", true, []string{"DASHBOARD", "PRIVATE_TESTNET", "LIVE", "REPLAY_MOCK"}, "/api/shell-status", in.Shell.DataDetail)
		default:
			add("canonical-market-data", "Canonical market-data / strategy universe", "BLOCKED", true, []string{"DASHBOARD", "PRIVATE_TESTNET", "LIVE", "REPLAY_MOCK"}, "/api/shell-status", in.Shell.DataDetail)
		}
		switch in.Shell.Reconciliation {
		case "CLEAN":
			add("reconciliation", "Reconciliation evidence", "PASS", false, []string{"PRIVATE_TESTNET", "LIVE", "REPLAY_MOCK"}, "/api/reconciliation", "Current retained evidence is CLEAN. Private venue reconciliation must still be re-proven after Steps 37–41.")
		case "BLOCKED":
			add("reconciliation", "Reconciliation evidence", "BLOCKED", true, []string{"DASHBOARD", "PRIVATE_TESTNET", "LIVE", "REPLAY_MOCK"}, "/api/reconciliation", "Current reconciliation evidence reports a contradiction/blocker.")
		case "DRIFT":
			add("reconciliation", "Reconciliation evidence", "WARN", false, []string{"PRIVATE_TESTNET", "LIVE", "REPLAY_MOCK"}, "/api/reconciliation", "Current reconciliation reports drift; no routing is enabled.")
		default:
			add("reconciliation", "Reconciliation evidence", "DEFERRED", false, []string{"PRIVATE_TESTNET", "LIVE", "REPLAY_MOCK"}, "/api/reconciliation", "Reconciliation is pending private/current venue evidence; Steps 38–41 remain deferred.")
		}
	}

	if in.PipelineErr != nil {
		add("durable-e2e-proof", "Durable end-to-end proof", "BLOCKED", true, []string{"DASHBOARD", "PRIVATE_TESTNET", "LIVE", "REPLAY_MOCK"}, "/api/pipeline", "Pipeline proof unavailable: "+in.PipelineErr.Error())
	} else if in.Pipeline.Proof == nil {
		add("durable-e2e-proof", "Durable end-to-end proof", "WARN", true, []string{"DASHBOARD", "PRIVATE_TESTNET", "LIVE", "REPLAY_MOCK"}, "/api/pipeline", "No Step 31 proof is exposed; quiet cycles are allowed but contradictions are not.")
	} else {
		switch in.Pipeline.Proof.Status {
		case "BLOCKED":
			add("durable-e2e-proof", "Durable end-to-end proof", "BLOCKED", true, []string{"DASHBOARD", "PRIVATE_TESTNET", "LIVE", "REPLAY_MOCK"}, "/api/pipeline", "Latest proof is BLOCKED ("+in.Pipeline.Proof.Completion+").")
		case "ALIGNED":
			add("durable-e2e-proof", "Durable end-to-end proof", "PASS", true, []string{"DASHBOARD", "PRIVATE_TESTNET", "LIVE", "REPLAY_MOCK"}, "/api/pipeline", "Latest proof is ALIGNED ("+in.Pipeline.Proof.Completion+").")
		default:
			add("durable-e2e-proof", "Durable end-to-end proof", "WARN", true, []string{"DASHBOARD", "PRIVATE_TESTNET", "LIVE", "REPLAY_MOCK"}, "/api/pipeline", "Latest proof is "+in.Pipeline.Proof.Status+" ("+in.Pipeline.Proof.Completion+"); a quiet/no-action cycle is acceptable.")
		}
	}

	if !in.FoundationOK || !in.Foundation.FoundationReady {
		detail := "Venue foundation provider is unavailable."
		if in.FoundationOK {
			detail = in.Foundation.Note
		}
		add("venue-foundation", "Hyperliquid TESTNET foundation", "BLOCKED", true, []string{"DASHBOARD", "PRIVATE_TESTNET"}, "/api/venue-foundation", detail)
	} else {
		add("venue-foundation", "Hyperliquid TESTNET foundation", "PASS", true, []string{"DASHBOARD", "PRIVATE_TESTNET"}, "/api/venue-foundation", in.Foundation.Venue+" / "+in.Foundation.TargetEnvironment+" / "+in.Foundation.GatewayMode)
	}

	if !in.VenuePublicOK || !in.VenuePublic.Connected {
		detail := "Public venue status provider unavailable."
		if in.VenuePublicOK {
			detail = in.VenuePublic.Error
			if strings.TrimSpace(detail) == "" {
				detail = in.VenuePublic.Note
			}
		}
		add("venue-public", "Public venue connectivity / metadata", "BLOCKED", true, []string{"DASHBOARD", "PRIVATE_TESTNET"}, "/api/venue-public", detail)
	} else {
		add("venue-public", "Public venue connectivity / metadata", "PASS", true, []string{"DASHBOARD", "PRIVATE_TESTNET"}, "/api/venue-public", fmt.Sprintf("%s %s public metadata is reachable; universe=%d mids=%d.", in.VenuePublic.Venue, in.VenuePublic.TargetEnvironment, in.VenuePublic.UniverseCount, in.VenuePublic.MidCount))
	}

	if !in.VenueRulesOK || !in.VenueRules.Validated {
		detail := "Venue trading-rules provider unavailable."
		if in.VenueRulesOK {
			detail = in.VenueRules.Error
			if strings.TrimSpace(detail) == "" {
				detail = in.VenueRules.Note
			}
		}
		add("venue-rules", "Public venue trading rules", "BLOCKED", true, []string{"DASHBOARD", "PRIVATE_TESTNET", "LIVE"}, "/api/venue-rules", detail)
	} else {
		add("venue-rules", "Public venue trading rules", "PASS", true, []string{"DASHBOARD", "PRIVATE_TESTNET", "LIVE"}, "/api/venue-rules", fmt.Sprintf("Validated rules %d/%d supported mappings; order routing remains disabled.", in.VenueRules.ValidatedRuleCount, in.VenueRules.SupportedMappingCount))
	}

	if !in.RegistryOK || !in.Registry.Validated {
		detail := "Symbol-registry provider unavailable."
		if in.RegistryOK {
			detail = in.Registry.Error
			if strings.TrimSpace(detail) == "" {
				detail = in.Registry.Note
			}
		}
		add("symbol-registry", "Multi-exchange symbol registry", "BLOCKED", true, []string{"DASHBOARD", "PRIVATE_TESTNET", "LIVE", "REPLAY_MOCK"}, "/api/symbol-registry", detail)
	} else {
		state := "PASS"
		detail := fmt.Sprintf("Registry %s classifies ranking %d/%d and strategy %d/%d; routable=%d/%d.", in.Registry.RegistryVersion, in.Registry.CurrentRankingRegisteredCount, in.Registry.CurrentRankingCount, in.Registry.CurrentStrategyRegisteredCount, in.Registry.CurrentStrategyCount, in.Registry.CurrentStrategyRoutableCount, in.Registry.CurrentStrategyCount)
		if !in.Registry.ExecutionCoverageComplete {
			state = "WARN"
			detail += " Coverage is intentionally partial; unsupported/venue-absent assets remain non-routable."
		}
		add("symbol-registry", "Multi-exchange symbol registry", state, true, []string{"DASHBOARD", "PRIVATE_TESTNET", "LIVE", "REPLAY_MOCK"}, "/api/symbol-registry", detail)
	}

	if !in.LedgerOK || !in.Ledger.Validated || !in.Ledger.FoundationReady {
		detail := "Ledger provider unavailable."
		if in.LedgerOK {
			detail = in.Ledger.Error
			if strings.TrimSpace(detail) == "" {
				detail = in.Ledger.Note
			}
		}
		add("ledger", "Append-only economic ledger foundation", "BLOCKED", true, []string{"DASHBOARD", "PRIVATE_TESTNET", "LIVE", "REPLAY_MOCK"}, "/api/ledger", detail)
	} else {
		add("ledger", "Append-only economic ledger foundation", "PASS", true, []string{"DASHBOARD", "PRIVATE_TESTNET", "LIVE", "REPLAY_MOCK"}, "/api/ledger", fmt.Sprintf("Validated %d durable fill row(s); projection is deterministic/read-only.", in.Ledger.TotalFillRows))
	}

	if in.AlertsErr != nil {
		add("watchdog", "Durable alert watchdog", "BLOCKED", true, []string{"DASHBOARD", "PRIVATE_TESTNET", "LIVE", "REPLAY_MOCK"}, "/api/alerts-audit", "Alerts/watchdog evidence unavailable: "+in.AlertsErr.Error())
		add("critical-alerts", "Active critical operational alerts", "BLOCKED", true, []string{"DASHBOARD", "PRIVATE_TESTNET", "LIVE", "REPLAY_MOCK"}, "/api/alerts-audit", "Critical-alert state cannot be verified.")
	} else {
		watchdogState := "BLOCKED"
		watchdogDetail := "Durable watchdog heartbeat is unavailable."
		if in.Alerts.WatchdogAvailable && in.Alerts.DurableAlertHistoryAvailable && strings.TrimSpace(in.Alerts.WatchdogLastSuccessAt) != "" {
			if ts, err := time.Parse(time.RFC3339, in.Alerts.WatchdogLastSuccessAt); err == nil {
				age := now.Sub(ts).Seconds()
				switch {
				case age > 120:
					watchdogState = "BLOCKED"
					watchdogDetail = fmt.Sprintf("Watchdog heartbeat is stale (%.0fs).", age)
				case strings.EqualFold(in.Alerts.WatchdogState, "HEALTHY"):
					watchdogState = "PASS"
					watchdogDetail = fmt.Sprintf("Durable watchdog heartbeat is fresh (%.0fs old).", age)
				case strings.EqualFold(in.Alerts.WatchdogState, "DEGRADED"):
					watchdogState = "WARN"
					watchdogDetail = fmt.Sprintf("Watchdog is DEGRADED but heartbeat is fresh (%.0fs old).", age)
				default:
					watchdogState = "BLOCKED"
					watchdogDetail = "Watchdog state is " + in.Alerts.WatchdogState + "."
				}
			} else {
				watchdogDetail = "Watchdog last-success timestamp is malformed."
			}
		}
		add("watchdog", "Durable alert watchdog", watchdogState, true, []string{"DASHBOARD", "PRIVATE_TESTNET", "LIVE", "REPLAY_MOCK"}, "/api/alerts-audit", watchdogDetail)

		if in.Alerts.ActiveCritical > 0 {
			detail := summarizeActiveCriticalAlerts(in.Alerts.Alerts, in.Alerts.ActiveCritical)
			add("critical-alerts", "Active critical operational alerts", "BLOCKED", true, []string{"DASHBOARD", "PRIVATE_TESTNET", "LIVE", "REPLAY_MOCK"}, "/api/alerts-audit", detail)
		} else {
			add("critical-alerts", "Active critical operational alerts", "PASS", true, []string{"DASHBOARD", "PRIVATE_TESTNET", "LIVE", "REPLAY_MOCK"}, "/api/alerts-audit", fmt.Sprintf("No active CRITICAL alerts; %d warning(s) remain observable.", in.Alerts.ActiveWarnings))
		}
	}

	if in.ManualErr != nil {
		add("manual-route", "Manual trading route", "BLOCKED", true, []string{"DASHBOARD", "PRIVATE_TESTNET", "LIVE", "REPLAY_MOCK"}, "/api/manual-control", "Manual route state cannot be verified: "+in.ManualErr.Error())
	} else if in.Manual.RouteEnabled {
		add("manual-route", "Manual trading route", "BLOCKED", true, []string{"DASHBOARD", "PRIVATE_TESTNET", "LIVE", "REPLAY_MOCK"}, "/api/manual-control", "routeEnabled=true while private auth/order lifecycle are still deferred.")
	} else if in.Manual.ContractVersion != provider.ManualControlContractVersion || !in.Manual.RoutingContractReady || !in.Manual.HumanAuditAvailable {
		add("manual-route", "Manual trading route", "BLOCKED", true, []string{"DASHBOARD", "PRIVATE_TESTNET", "LIVE", "REPLAY_MOCK"}, "/api/manual-control", "Step 46 manual routing admission/audit contract is not fully available.")
	} else {
		add("manual-route", "Manual trading route", "PASS", true, []string{"DASHBOARD"}, "/api/manual-control", "Step 46 confirmed/hash-bound/stale-reference-checked routing admission contract and durable operator-intent audit are available; actual routing remains fail-closed until PortfolioRisk manual transformation, trading-control sink, private auth and order lifecycle are proven.")
	}

	if in.DemoAuth {
		add("production-auth", "Production authentication posture", "WARN", false, []string{"LIVE"}, "server config", "Demo credentials are enabled for local WSL development and must be disabled before production/testnet secrets are introduced.")
	} else if !in.CookieSecure {
		add("production-auth", "Production authentication posture", "WARN", false, []string{"LIVE"}, "server config", "Demo credentials are disabled, but Secure cookie is not enabled; production HTTPS must enable it.")
	} else {
		add("production-auth", "Production authentication posture", "PASS", false, []string{"LIVE"}, "server config", "Demo credentials are disabled and Secure cookies are enabled.")
	}

	add("private-auth", "Hyperliquid private API wallet authentication", "DEFERRED", false, []string{"PRIVATE_TESTNET", "LIVE"}, "Step 37", "Deferred until the dashboard is complete; no wallet/funds are required for Steps 44–46.")
	add("account-snapshot", "Private account / balances / open orders", "DEFERRED", false, []string{"PRIVATE_TESTNET", "LIVE"}, "Steps 38–41", "Requires the deferred private Hyperliquid account boundary.")
	add("order-lifecycle", "Submit / cancel / fills / private reconciliation", "DEFERRED", false, []string{"PRIVATE_TESTNET", "LIVE"}, "Steps 39–41", "No private signing or order command path exists yet; routing must remain disabled.")
	add("service-liveness", "Common trading-service liveness/control contract", "DEFERRED", false, []string{"PRIVATE_TESTNET", "LIVE", "REPLAY_MOCK"}, "future runtime contract", "Durable state is observed, but process liveness/paused state is not yet independently authoritative.")
	add("clock-sync", "Host clock synchronization", "DEFERRED", false, []string{"LIVE"}, "future host/VPS telemetry", "Host clock sync is not inferred from inside the dashboard container.")
	add("mock-exchange", "Mock exchange adapter for full replay + dashboard", "DEFERRED", false, []string{"REPLAY_MOCK"}, "future Venue Adapter boundary", "After Hyperliquid private lifecycle integration, add MockExchangeAdapter behind the same canonical venue contract; do not create a dashboard-only replay shortcut.")

	for i := range requirements {
		requirements[i].Retryable = globalReadinessRetryable(requirements[i])
		if requirements[i].ID == "critical-alerts" && requirements[i].State == "BLOCKED" && allActiveCriticalAlertsTransient(in.Alerts.Alerts, in.Alerts.ActiveCritical) {
			requirements[i].Retryable = true
		}
	}

	expectedIDs := []string{
		"provider-real", "postgres", "nats", "canonical-market-data", "reconciliation",
		"durable-e2e-proof", "venue-foundation", "venue-public", "venue-rules", "symbol-registry",
		"ledger", "watchdog", "critical-alerts", "manual-route", "production-auth", "private-auth",
		"account-snapshot", "order-lifecycle", "service-liveness", "clock-sync", "mock-exchange",
	}
	seen := make(map[string]bool, len(requirements))
	complete := true
	blockers := make([]string, 0)
	warnings := make([]string, 0)
	deferred := make([]string, 0)
	retryableBlockingCount := 0
	for _, req := range requirements {
		if seen[req.ID] {
			complete = false
		}
		seen[req.ID] = true
		switch req.State {
		case "BLOCKED":
			if req.RequiredNow {
				blockers = append(blockers, req.Label+": "+req.Detail)
				if req.Retryable {
					retryableBlockingCount++
				}
			} else {
				warnings = append(warnings, req.Label+": "+req.Detail)
			}
		case "WARN":
			warnings = append(warnings, req.Label+": "+req.Detail)
		case "DEFERRED":
			deferred = append(deferred, req.Label+": "+req.Detail)
		case "PASS":
		default:
			complete = false
		}
	}
	for _, id := range expectedIDs {
		if !seen[id] {
			complete = false
		}
	}

	sort.Strings(blockers)
	sort.Strings(warnings)
	sort.Strings(deferred)

	safeDashboard := complete && len(blockers) == 0
	status := "BLOCKED"
	if safeDashboard {
		status = "VALIDATED_FAIL_CLOSED"
	}

	privateReady := false
	tradingReady := false
	liveReady := false

	orderRouting := "DISABLED"
	privateAuth := "DEFERRED"
	if in.FoundationOK {
		if strings.TrimSpace(in.Foundation.OrderRouting) != "" {
			orderRouting = in.Foundation.OrderRouting
		}
		if strings.TrimSpace(in.Foundation.PrivateAuth) != "" && !strings.EqualFold(in.Foundation.PrivateAuth, "DISABLED") {
			privateAuth = in.Foundation.PrivateAuth
		}
	}
	manualRouting := "UNKNOWN"
	if in.ManualErr == nil {
		if in.Manual.RouteEnabled {
			manualRouting = "ENABLED"
		} else {
			manualRouting = "DISABLED"
		}
	}

	return globalReadinessResponse{
		ContractVersion:         globalReadinessContractVersion,
		Status:                  status,
		ContractComplete:        complete,
		CurrentPhase:            "DASHBOARD_COMPLETE_PRIVATE_TESTNET_DEFERRED",
		SafeToContinueDashboard: safeDashboard,
		PrivateTestnetReady:     privateReady,
		TradingReady:            tradingReady,
		LiveReady:               liveReady,
		OrderRouting:            orderRouting,
		PrivateAuth:             privateAuth,
		ManualRouting:           manualRouting,
		BlockingCount:           len(blockers),
		WarningCount:            len(warnings),
		DeferredCount:           len(deferred),
		RetryableBlockingCount:  retryableBlockingCount,
		Blockers:                blockers,
		Warnings:                warnings,
		Deferred:                deferred,
		Requirements:            requirements,
		CheckedAt:               now.Format(time.RFC3339),
		NextSafeStep:            "STEP_37_PRIVATE_TESTNET_AUTH",
		FutureReplayBoundary:    "HyperliquidAdapter and future MockExchangeAdapter must share the same canonical venue adapter contract so replay can drive the full dashboard without a special UI path.",
		Note:                    "Step 44 readiness now reflects the completed Step 46 dashboard contract. Dashboard observability/control admission is complete for this phase, but private testnet, trading and LIVE readiness remain false until the deferred Hyperliquid private lifecycle and normal trading-control path are actually proven.",
	}
}

func isTransientReadBlockerDetail(detail string) bool {
	detail = strings.ToLower(strings.TrimSpace(detail))
	for _, marker := range []string{
		"timed out",
		"timeout",
		"context deadline exceeded",
		"connection reset",
		"connection refused",
		"i/o timeout",
		"network is unreachable",
		"no route to host",
		"temporarily unavailable",
		"read model unavailable",
		"source unavailable",
		"metadata unavailable",
		"ranking unavailable",
		"strategy universe unavailable",
		"database is locked",
		"database is busy",
	} {
		if strings.Contains(detail, marker) {
			return true
		}
	}
	return false
}

func globalReadinessRetryable(req globalReadinessRequirement) bool {
	if req.State != "BLOCKED" || !req.RequiredNow {
		return false
	}
	switch req.ID {
	case "postgres", "nats", "canonical-market-data", "durable-e2e-proof", "venue-public", "venue-rules", "symbol-registry", "ledger", "manual-route":
	default:
		return false
	}
	return isTransientReadBlockerDetail(req.Detail)
}

func activeCriticalAlerts(alerts []readinessOperationalAlert) []readinessOperationalAlert {
	result := make([]readinessOperationalAlert, 0)
	for _, alert := range alerts {
		if strings.EqualFold(strings.TrimSpace(alert.Status), "ACTIVE") && strings.EqualFold(strings.TrimSpace(alert.Severity), "CRITICAL") {
			result = append(result, alert)
		}
	}
	return result
}

func allActiveCriticalAlertsTransient(alerts []readinessOperationalAlert, declaredCount int) bool {
	critical := activeCriticalAlerts(alerts)
	if declaredCount <= 0 || len(critical) != declaredCount {
		// Missing alert detail is not sufficient evidence to downgrade/retry a
		// critical condition. Stay fail-closed for backward compatibility.
		return false
	}
	for _, alert := range critical {
		service := strings.ToUpper(strings.TrimSpace(alert.Service))
		if alert.EventType != "SOURCE_CRITICAL" || (service != "NATS" && service != "POSTGRESQL") || !isTransientReadBlockerDetail(alert.Detail) {
			return false
		}
	}
	return true
}

func summarizeActiveCriticalAlerts(alerts []readinessOperationalAlert, declaredCount int) string {
	critical := activeCriticalAlerts(alerts)
	if len(critical) == 0 {
		return fmt.Sprintf("%d active critical alert(s) require resolution; alert detail was unavailable, so readiness remains fail-closed.", declaredCount)
	}
	parts := make([]string, 0, len(critical))
	for _, alert := range critical {
		detail := strings.TrimSpace(alert.Detail)
		if len(detail) > 180 {
			detail = detail[:180] + "…"
		}
		parts = append(parts, fmt.Sprintf("%s/%s: %s", alert.Service, alert.EventType, detail))
	}
	return fmt.Sprintf("%d active critical alert(s) require resolution: %s", declaredCount, strings.Join(parts, "; "))
}
