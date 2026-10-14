package provider

import (
	"context"
	"encoding/json"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"time"

	pgstore "control-dashboard-api/internal/integration/postgres"
	"control-dashboard-api/internal/manualaudit"
	"control-dashboard-api/internal/tradingwire"
)

// Manual Control for the trading chain (PAPER and Kraken LIVE).
//
// The dashboard validates the CSV against the portfolio risk limits, previews it at the
// latest daily closes and, on confirmation, writes a request file to
// <ManualRequestDir>/requests. The execution-state service picks it up within a second,
// checks everything again, sizes the orders at the current Binance prices and sends them
// through the normal exchange path (simulated exchange in PAPER, Kraken in LIVE). The
// next daily cycle returns to the strategy targets. The file format is described in
// live_trading/execution_state_service/src/manual_requests.h.

const (
	chainManualContractVersion = "chain-manual-v1"
	chainManualHeader          = "MANUAL_REQUEST_V1"
	// Execution-state reads the request as another user (see deploy/chain/compose.yml).
	chainManualRequestMode = 0o644
)

// Order statuses of the persisted execution state (ExecutionOrderStatus in C++).
const (
	orderStatusFilled   = 4
	orderStatusCanceled = 5
	orderStatusRejected = 6
)

func (p *Real) chainManualEnabled() bool {
	return p.currentDataChain() && strings.TrimSpace(p.cfg.ManualRequestDir) != ""
}

// chainManualView is what the page and the preview need from the running chain.
type chainManualView struct {
	limits     manualLimits
	limitsOK   bool
	routable   map[string]bool
	current    map[string]float64 // % of equity at the latest closes, CASH included
	equity     float64
	closeDate  uint64
	state      *tradingwire.TradingStateSnapshot
	blockers   []string
	exchange   string
	unroutable string
}

func (p *Real) loadChainManualView(ctx context.Context) chainManualView {
	view := chainManualView{routable: map[string]bool{}, current: map[string]float64{}, exchange: "SIMULATED",
		unroutable: "Not in the current top-" + strconv.Itoa(p.cfg.MarketTopN) + " market universe or held positions."}
	if p.krakenLive() {
		view.exchange = "KRAKEN"
		view.unroutable = "No verified Kraken contract for this asset (Symbol Mapping page)."
		if !strings.EqualFold(strings.TrimSpace(p.cfg.LiveOrderSubmission), "enabled") {
			view.blockers = append(view.blockers, "LIVE_ORDER_SUBMISSION_DISABLED")
		}
	}

	if checkpoint, err := p.postgres.LatestPipelineCheckpoint(ctx); err == nil {
		view.limits, view.limitsOK = portfolioManualLimits(checkpoint.PortfolioConfig)
	}
	if !view.limitsOK {
		view.blockers = append(view.blockers, "RISK_LIMITS_UNAVAILABLE")
	}

	runtime, err := p.postgres.RuntimeState(ctx)
	if err != nil {
		view.blockers = append(view.blockers, "EXECUTION_STATE_UNAVAILABLE")
	} else {
		view.state = &runtime.Snapshot
		for _, order := range runtime.Snapshot.Orders {
			if order.Status != orderStatusFilled && order.Status != orderStatusCanceled && order.Status != orderStatusRejected {
				view.blockers = append(view.blockers, "ORDERS_IN_FLIGHT")
				break
			}
		}
	}

	// Tradable assets: Kraken contracts verified against prices in LIVE, the market
	// universe in PAPER; held assets can always be closed.
	if p.krakenLive() {
		for _, row := range p.SymbolMapStatus(ctx).Rows {
			if row.State == symbolMapStateOK {
				view.routable[row.Source] = true
			}
		}
	} else if window, err := p.marketData.LoadStrategyWindow(ctx, p.cfg.MarketTopN, 1); err == nil {
		for _, row := range window.Ranking {
			view.routable[row.Pair] = true
		}
	}
	pairs := []string{}
	if view.state != nil {
		for asset, quantity := range view.state.AccountPositions {
			if quantity != 0 {
				view.routable[asset] = true
			}
		}
	}
	for asset := range view.routable {
		pairs = append(pairs, asset)
	}
	sort.Strings(pairs)

	if view.state != nil {
		closes, _ := p.marketData.LatestCloses(ctx, pairs)
		equity := view.state.AccountCash
		if p.krakenLive() {
			// Same capital as execution-state: BTC collateral after haircut counts, though it
			// is not cash, so the asset and CASH weights then add up to less than 100%.
			account, accountErr := p.readKrakenAccount(time.Now().UTC())
			var days []pgstore.EquityDay
			if accounting, err := p.postgres.Accounting(ctx); err == nil && accounting.Available {
				days = accounting.Equity
			}
			if collateral, ok := latestCollateral(account, accountErr, days); ok {
				equity += collateral
			}
		}
		for asset, quantity := range view.state.AccountPositions {
			if close, ok := closes[asset]; ok {
				equity += quantity * close.Close
				view.closeDate = max(view.closeDate, close.Date)
			}
		}
		view.equity = equity
		if equity > 0 {
			for asset, quantity := range view.state.AccountPositions {
				if close, ok := closes[asset]; ok && quantity != 0 {
					view.current[asset] = quantity * close.Close / equity * 100
				}
			}
			view.current["CASH"] = view.state.AccountCash / equity * 100
		} else {
			view.blockers = append(view.blockers, "EQUITY_UNAVAILABLE")
		}
	}

	if inDailyCycleWindow(time.Now().UTC()) {
		view.blockers = append(view.blockers, "DAILY_CYCLE_WINDOW")
	}
	if p.chainManualQueued() {
		view.blockers = append(view.blockers, "REQUEST_IN_PROGRESS")
	}
	view.blockers = uniqueStrings(view.blockers)
	return view
}

// Same window as execution-state: no manual orders from 23:45 to 00:15 UTC.
func inDailyCycleWindow(now time.Time) bool {
	minutes := now.Hour()*60 + now.Minute()
	return minutes >= 23*60+45 || minutes < 15
}

// portfolioManualLimits reads the strictest strategy limits from the risk config
// identity (the JSON document followed by service settings).
func portfolioManualLimits(identity string) (manualLimits, bool) {
	policies := checkpointRiskPolicyLimits(identity)
	if len(policies) == 0 {
		return manualLimits{}, false
	}
	limits := manualLimits{grossPct: math.Inf(1), assetPct: math.Inf(1)}
	for _, policy := range policies {
		limits.grossPct = math.Min(limits.grossPct, policy.gross*100)
		limits.assetPct = math.Min(limits.assetPct, policy.asset*100)
	}
	return limits, limits.grossPct > 0 && limits.assetPct > 0
}

func (p *Real) chainManualBase(ctx context.Context, actor string) (ManualControlData, chainManualView) {
	view := p.loadChainManualView(ctx)
	safety := "PAPER: executes at once at the current Binance prices in the simulated account; at 00:00 UTC the daily cycle returns to the strategy targets."
	privateAuth := "DISABLED"
	if view.exchange == "KRAKEN" {
		safety = "LIVE: sends real orders to Kraken at the market now; at 00:00 UTC the daily cycle returns to the strategy targets. Weights are % of the capital with the BTC collateral after haircut, as the strategy sizes."
		privateAuth = "KRAKEN_TRADING_KEY"
	}
	result := ManualControlData{
		ContractVersion: chainManualContractVersion, RoutingContractReady: true,
		RoutingContractMode: "CHAIN_MANUAL_REBALANCE",
		ExecutionBoundary:   "OPERATOR -> dashboard-api -> request file -> execution-state -> planner -> gateway -> " + view.exchange,
		TradingControlSink:  "CHAIN_REQUEST_FILES", PrivateAuth: privateAuth, OrderLifecycle: "CHAIN",
		ConfirmationRequired: true, ConfirmationPhrase: ManualControlConfirmationPhrase,
		HumanAuditAvailable: true, RouteBlockers: view.blockers, RecentRouteAudits: []ManualRouteAuditRow{},
		Mode: p.runtimeMode(), Exchange: view.exchange, SchemaLabel: "asset,weight_pct", MaxUploadSizeLabel: "256 KB",
		ExampleCSV: chainManualExample(view), PreviewRows: []ManualPreviewRow{}, ValidationIssues: []ManualValidationIssue{},
		OrderPreview: []ManualOrderPreviewRow{}, EstimatedFeesLabel: "—", EstimatedTurnoverLabel: "—",
		AuditActorLabel: actor, BackendAuthoritative: true, RiskCheckAvailable: view.limitsOK,
		RouteEnabled: len(view.blockers) == 0, SourceMode: "REAL",
		CurrentTargetTimestamp: strconv.FormatUint(view.closeDate, 10), PreviewKind: "TARGET_WEIGHTS",
		SafetyNote: safety, AcceptingRequests: len(view.blockers) == 0,
		MaxGrossPct: view.limits.grossPct, MaxAssetPct: view.limits.assetPct,
		Requests: p.chainManualRequestStatuses(view.state),
	}
	return result, view
}

// chainManualPreview validates the CSV and returns the page with its preview plus the
// validated targets (% of equity).
func (p *Real) chainManualPreview(ctx context.Context, req ManualControlPreviewRequest) (ManualControlData, map[string]float64) {
	result, view := p.chainManualBase(ctx, req.Actor)
	if !view.limitsOK {
		result.ValidationIssues = []ManualValidationIssue{{Severity: "ERROR", Field: "risk", Message: "The portfolio risk limits are not available yet (no risk decision in PostgreSQL)."}}
		result.RouteEnabled = false
		return result, nil
	}
	targets, cash, issues := parseManualTargetCSV(req.CSV, view.routable, view.limits, view.unroutable)
	result.ValidationIssues = issues
	result.ValidationPassed = !hasManualError(issues)
	result.ExchangeConstraintsValidated = result.ValidationPassed
	if !result.ValidationPassed {
		result.RouteEnabled = false
		result.RouteBlockers = uniqueStrings(append(result.RouteBlockers, "PREVIEW_VALIDATION_FAILED"))
		return result, nil
	}
	fillManualPreview(&result, targets, cash, view.current, view.equity)
	return result, targets
}

// RouteManualControl re-validates the CSV, checks it is the exact previewed request and
// writes it for execution-state, which checks it again before any order.
func (p *Real) RouteManualControl(ctx context.Context, req SimulationManualRouteRequest) (SimulationManualRouteResult, error) {
	result := SimulationManualRouteResult{
		ContractVersion: chainManualContractVersion, Status: "BLOCKED", ConfirmationAccepted: true,
		RequestHash: req.RequestHash, Actor: req.Actor, ReferenceTargetTimestamp: req.ReferenceTargetTimestamp,
		CheckedAt: time.Now().UTC().Format(time.RFC3339Nano), Blockers: []string{},
	}
	if !p.chainManualEnabled() {
		result.Blockers = []string{"MANUAL_CONTROL_NOT_CONFIGURED"}
		result.Note = "Manual Control is only available in the PAPER or Kraken LIVE chain with a request folder."
		return result, nil
	}

	preview, targets := p.chainManualPreview(ctx, ManualControlPreviewRequest{Actor: req.Actor, Filename: req.Filename, CSV: req.CSV})
	result.RouteEnabled = preview.RouteEnabled
	result.Blockers = append(result.Blockers, preview.RouteBlockers...)
	if req.RequestHash == "" || req.RequestHash != preview.RequestHash {
		result.Blockers = append(result.Blockers, "REQUEST_HASH_MISMATCH")
	}
	result.Blockers = uniqueStrings(result.Blockers)
	if !preview.ValidationPassed || !preview.RouteEnabled || len(result.Blockers) > 0 {
		result.Note = "Request not sent."
		p.auditManualRequest(result, "REJECTED", "Manual request blocked: "+strings.Join(result.Blockers, ", "))
		return result, nil
	}

	id, err := writeChainManualRequest(p.cfg.ManualRequestDir, req.Actor, preview.RequestHash, targets)
	if err != nil {
		return SimulationManualRouteResult{}, err
	}
	result.Status = "QUEUED"
	result.Submitted = true
	result.CorrelationID = id
	result.Note = "Sent. Execution-state checks it and sends the orders within a few seconds."
	result.AuditPersisted = p.auditManualRequest(result, "QUEUED", "Manual request "+id+" sent to execution-state ("+preview.Exchange+")")
	return result, nil
}

func (p *Real) auditManualRequest(result SimulationManualRouteResult, outcome, detail string) bool {
	eventID := result.CorrelationID
	if eventID == "" {
		eventID, _ = manualRequestID()
	}
	err := manualaudit.New(p.cfg.ManualAuditDir).Append(manualaudit.Event{
		EventID: eventID, Actor: result.Actor, Action: "MANUAL_REBALANCE", Target: "portfolio", Result: outcome,
		RequestHash: result.RequestHash, CorrelationID: eventID, ReferenceTargetTimestamp: result.ReferenceTargetTimestamp,
		ContractVersion: chainManualContractVersion, Submitted: outcome == "QUEUED", Blockers: result.Blockers, Detail: detail,
	})
	return err == nil
}

// writeChainManualRequest writes MANUAL_REQUEST_V1 atomically (tmp + rename). The name
// starts with the queue time in nanoseconds so execution-state keeps arrival order.
func writeChainManualRequest(dir, actor, hash string, targets map[string]float64) (string, error) {
	id, err := manualRequestID()
	if err != nil {
		return "", err
	}
	requests := filepath.Join(dir, manualRequestsSubdir)
	var body strings.Builder
	body.WriteString(chainManualHeader + "\n")
	fmt.Fprintf(&body, "request_id=%s\n", id)
	fmt.Fprintf(&body, "actor=%s\n", sanitizeLine(actor))
	fmt.Fprintf(&body, "request_hash=%s\n", hash)
	fmt.Fprintf(&body, "created_at=%s\n", time.Now().UTC().Format("2006-01-02T15:04:05Z"))
	assets := make([]string, 0, len(targets))
	for asset := range targets {
		assets = append(assets, asset)
	}
	sort.Strings(assets)
	for _, asset := range assets {
		fmt.Fprintf(&body, "target=%s,%.12f\n", asset, targets[asset]/100)
	}

	temp, err := os.CreateTemp(requests, ".manual-*.tmp")
	if err != nil {
		return "", err
	}
	cleanup := func(err error) (string, error) {
		temp.Close()
		os.Remove(temp.Name())
		return "", err
	}
	if _, err := temp.WriteString(body.String()); err != nil {
		return cleanup(err)
	}
	if err := temp.Chmod(chainManualRequestMode); err != nil {
		return cleanup(err)
	}
	if err := temp.Sync(); err != nil {
		return cleanup(err)
	}
	if err := temp.Close(); err != nil {
		os.Remove(temp.Name())
		return "", err
	}
	final := filepath.Join(requests, fmt.Sprintf("%019d-%s.request", time.Now().UnixNano(), id))
	if err := os.Rename(temp.Name(), final); err != nil {
		os.Remove(temp.Name())
		return "", err
	}
	return id, nil
}

// chainManualQueued reports a request still waiting for execution-state.
func (p *Real) chainManualQueued() bool {
	entries, err := os.ReadDir(filepath.Join(p.cfg.ManualRequestDir, manualRequestsSubdir))
	if err != nil {
		return false
	}
	for _, entry := range entries {
		if !entry.IsDir() && strings.HasSuffix(entry.Name(), ".request") {
			return true
		}
	}
	return false
}

// chainManualRequestStatuses follows recent requests: queued -> checked by
// execution-state -> orders at the exchange -> filled -> back to the strategy targets
// after the next daily cycle. Newest first.
func (p *Real) chainManualRequestStatuses(state *tradingwire.TradingStateSnapshot) []ManualRequestStatus {
	statuses := []ManualRequestStatus{}
	type file struct{ name, dir string }
	files := []file{}
	for _, dir := range []string{manualRequestsSubdir, manualProcessedSubdir} {
		entries, err := os.ReadDir(filepath.Join(p.cfg.ManualRequestDir, dir))
		if err != nil {
			continue
		}
		for _, entry := range entries {
			if !entry.IsDir() && strings.HasSuffix(entry.Name(), ".request") {
				files = append(files, file{entry.Name(), dir})
			}
		}
	}
	sort.Slice(files, func(i, j int) bool { return files[i].name > files[j].name }) // queue time prefix
	if len(files) > manualStatusLimit {
		files = files[:manualStatusLimit]
	}

	orders := map[uint64]tradingwire.PersistedTrackedOrder{}
	lastExecution := uint64(0)
	if state != nil {
		for _, order := range state.Orders {
			orders[order.OrderID] = order
		}
		lastExecution = state.LastExecutionTimestamp
	}

	for _, f := range files {
		fields, _ := readManualRecord(filepath.Join(p.cfg.ManualRequestDir, f.dir, f.name))
		status := ManualRequestStatus{ID: fields["request_id"], Stage: "QUEUED", Detail: "Waiting for execution-state (a few seconds)"}
		if f.dir == manualProcessedSubdir {
			status = chainManualStatus(status, fields, orders, lastExecution)
		}
		statuses = append(statuses, status)
	}
	return statuses
}

func chainManualStatus(status ManualRequestStatus, fields map[string]string, orders map[uint64]tradingwire.PersistedTrackedOrder, lastExecution uint64) ManualRequestStatus {
	status.Detail = fields["detail"]
	if fields["result"] != "EXECUTED" {
		status.Stage = "FAILED"
		return status
	}
	status.Stage = "EXECUTED"
	executionDay, _ := strconv.ParseUint(fields["execution_day"], 10, 64)
	resynced := executionDay != 0 && lastExecution > executionDay
	first, last, ok := parseOrderRange(fields["orders"])
	if !ok {
		status.Stage = "RESYNCED" // nothing to trade: already at the target
		return status
	}

	rejected := []string{}
	terminal := 0
	for id := first; id <= last; id++ {
		order, seen := orders[id]
		if !seen {
			continue
		}
		status.Orders++
		switch order.Status {
		case orderStatusFilled:
			status.Filled++
			terminal++
		case orderStatusCanceled, orderStatusRejected:
			terminal++
			if order.LastMessage != "" {
				rejected = append(rejected, order.Coin+": "+order.LastMessage)
			} else {
				rejected = append(rejected, order.Coin)
			}
		}
	}
	switch {
	case status.Orders == 0:
		status.Detail = "Sending the orders"
	case terminal < status.Orders:
		status.Stage = "AT_EXCHANGE"
	case status.Filled < status.Orders:
		status.Stage = "FAILED"
		status.Detail = fmt.Sprintf("%d of %d orders filled; not filled: %s", status.Filled, status.Orders, strings.Join(rejected, "; "))
	case resynced:
		status.Stage = "RESYNCED"
	default:
		status.Stage = "FILLED"
		status.Detail = "Filled. The 00:00 UTC daily cycle returns to the strategy targets."
	}
	return status
}

func chainManualExample(view chainManualView) string {
	picks := []string{}
	for _, asset := range []string{"BTCUSDT", "ETHUSDT"} {
		if view.routable[asset] {
			picks = append(picks, asset)
		}
	}
	var b strings.Builder
	b.WriteString("asset,weight_pct\n")
	used := 0
	for i, asset := range picks {
		weight := []int{20, 10}[i]
		fmt.Fprintf(&b, "%s,%d\n", asset, weight)
		used += weight
	}
	fmt.Fprintf(&b, "CASH,%d", 100-used)
	return b.String()
}

type riskPolicyLimit struct{ gross, asset float64 }

// checkpointRiskPolicyLimits reads each strategy's max_gross_leverage and
// max_asset_weight from the risk config identity; empty when any is missing.
func checkpointRiskPolicyLimits(identity string) []riskPolicyLimit {
	var config struct {
		Strategies []struct {
			Risk struct {
				Gross *float64 `json:"max_gross_leverage"`
				Asset *float64 `json:"max_asset_weight"`
			} `json:"risk"`
		} `json:"strategies"`
	}
	// Service identity appends mode/history settings after the JSON document.
	if json.NewDecoder(strings.NewReader(identity)).Decode(&config) != nil {
		return nil
	}
	limits := []riskPolicyLimit{}
	for _, strategy := range config.Strategies {
		if strategy.Risk.Gross == nil || strategy.Risk.Asset == nil || *strategy.Risk.Gross <= 0 || *strategy.Risk.Asset <= 0 {
			return nil
		}
		limits = append(limits, riskPolicyLimit{*strategy.Risk.Gross, *strategy.Risk.Asset})
	}
	return limits
}
