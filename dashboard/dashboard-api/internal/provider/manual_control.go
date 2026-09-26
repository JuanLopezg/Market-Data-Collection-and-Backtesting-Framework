package provider

import (
	"context"
	"crypto/sha256"
	"encoding/csv"
	"fmt"
	"math"
	"regexp"
	"sort"
	"strconv"
	"strings"

	pgstore "control-dashboard-api/internal/integration/postgres"
	"control-dashboard-api/internal/manualaudit"
)

const (
	manualControlMaxCSVBytes        = 256 * 1024
	ManualControlContractVersion    = "step46-v1"
	ManualControlConfirmationPhrase = "CONFIRM_MANUAL_ROUTE"
)

var manualAssetPattern = regexp.MustCompile(`^[A-Z0-9]{2,24}$`)

type ManualControlPreviewRequest struct {
	Actor    string
	Filename string
	CSV      string
}

type ManualValidationIssue struct {
	Severity string `json:"severity"`
	Field    string `json:"field"`
	Message  string `json:"message"`
}

type ManualPreviewRow struct {
	Asset                  string  `json:"asset"`
	CurrentWeightPct       float64 `json:"currentWeightPct"`
	RequestedWeightPct     float64 `json:"requestedWeightPct"`
	ApprovedWeightPct      float64 `json:"approvedWeightPct"`
	EstimatedNotionalLabel string  `json:"estimatedNotionalLabel"`
	DeltaPct               float64 `json:"deltaPct"`
	RiskNote               string  `json:"riskNote,omitempty"`
}

type ManualOrderPreviewRow struct {
	Asset                  string  `json:"asset"`
	Action                 string  `json:"action"`
	DeltaWeightPct         float64 `json:"deltaWeightPct"`
	EstimatedNotionalLabel string  `json:"estimatedNotionalLabel"`
	EstimatedNotionalUSD   float64 `json:"estimatedNotionalUsd"`
	EstimatedFeeLabel      string  `json:"estimatedFeeLabel"`
	Note                   string  `json:"note"`
}

type ManualRouteAuditRow struct {
	RecordedAt               string   `json:"recordedAt"`
	Actor                    string   `json:"actor"`
	Result                   string   `json:"result"`
	RequestHash              string   `json:"requestHash"`
	CorrelationID            string   `json:"correlationId"`
	ReferenceTargetTimestamp string   `json:"referenceTargetTimestamp"`
	Submitted                bool     `json:"submitted"`
	Blockers                 []string `json:"blockers"`
	Detail                   string   `json:"detail"`
}

type ManualControlData struct {
	ContractVersion              string                  `json:"contractVersion"`
	RoutingContractReady         bool                    `json:"routingContractReady"`
	RoutingContractMode          string                  `json:"routingContractMode"`
	ExecutionBoundary            string                  `json:"executionBoundary"`
	ExchangeConstraintsValidated bool                    `json:"exchangeConstraintsValidated"`
	RegistryVersion              string                  `json:"registryVersion,omitempty"`
	RegistryArtifactSHA256       string                  `json:"registryArtifactSha256,omitempty"`
	VenueRulesCheckedAt          string                  `json:"venueRulesCheckedAt,omitempty"`
	TradingControlSink           string                  `json:"tradingControlSink"`
	PrivateAuth                  string                  `json:"privateAuth"`
	OrderLifecycle               string                  `json:"orderLifecycle"`
	ConfirmationRequired         bool                    `json:"confirmationRequired"`
	ConfirmationPhrase           string                  `json:"confirmationPhrase"`
	HumanAuditAvailable          bool                    `json:"humanAuditAvailable"`
	RouteBlockers                []string                `json:"routeBlockers"`
	RecentRouteAudits            []ManualRouteAuditRow   `json:"recentRouteAudits"`
	Mode                         string                  `json:"mode"`
	Exchange                     string                  `json:"exchange"`
	SchemaLabel                  string                  `json:"schemaLabel"`
	MaxUploadSizeLabel           string                  `json:"maxUploadSizeLabel"`
	ExampleCSV                   string                  `json:"exampleCsv"`
	RequestHash                  string                  `json:"requestHash"`
	PreviewRows                  []ManualPreviewRow      `json:"previewRows"`
	ValidationIssues             []ManualValidationIssue `json:"validationIssues"`
	OrderPreview                 []ManualOrderPreviewRow `json:"orderPreview"`
	EstimatedFeesLabel           string                  `json:"estimatedFeesLabel"`
	EstimatedTurnoverLabel       string                  `json:"estimatedTurnoverLabel"`
	AuditActorLabel              string                  `json:"auditActorLabel"`
	BackendAuthoritative         bool                    `json:"backendAuthoritative"`
	ValidationPassed             bool                    `json:"validationPassed"`
	RiskCheckAvailable           bool                    `json:"riskCheckAvailable"`
	RouteEnabled                 bool                    `json:"routeEnabled"`
	SourceMode                   string                  `json:"sourceMode"`
	CurrentTargetTimestamp       string                  `json:"currentTargetTimestamp"`
	PreviewKind                  string                  `json:"previewKind"`
	SafetyNote                   string                  `json:"safetyNote"`
}

func (p *Real) manualControlBase(ctx context.Context) (ManualControlData, error) {
	checkpoint, err := p.postgres.LatestPipelineCheckpoint(ctx)
	if err != nil {
		return ManualControlData{}, fmt.Errorf("read manual-control reference checkpoint: %w", err)
	}
	example := buildManualExample(checkpoint)
	result := ManualControlData{
		Mode:                   p.runtimeMode(),
		Exchange:               "NORMAL PIPELINE",
		SchemaLabel:            "asset,weight_pct",
		MaxUploadSizeLabel:     "256 KB",
		ExampleCSV:             example,
		PreviewRows:            []ManualPreviewRow{},
		ValidationIssues:       []ManualValidationIssue{{Severity: "INFO", Field: "backend", Message: "CSV validation, exchange-admission checks and route confirmation run server-side under the authenticated OPERATOR session."}},
		OrderPreview:           []ManualOrderPreviewRow{},
		EstimatedFeesLabel:     "Not estimated",
		EstimatedTurnoverLabel: "—",
		AuditActorLabel:        "authenticated OPERATOR session",
		BackendAuthoritative:   true,
		ValidationPassed:       false,
		RiskCheckAvailable:     false,
		RouteEnabled:           false,
		SourceMode:             "REAL",
		CurrentTargetTimestamp: formatTradingTimestamp(checkpoint.Timestamp),
		PreviewKind:            "TARGET_DELTA_ONLY",
	}
	p.decorateManualControlContract(&result)
	return result, nil
}

func (p *Real) decorateManualControlContract(result *ManualControlData) {
	if result == nil {
		return
	}
	result.ContractVersion = ManualControlContractVersion
	result.RoutingContractMode = "ADMISSION_ONLY_FAIL_CLOSED"
	result.ExecutionBoundary = "OPERATOR -> dashboard admission -> future trading-control service -> PortfolioRisk -> OrderPlanner -> ExecutionState -> VenueAdapter"
	result.TradingControlSink = "UNCONFIGURED"
	result.PrivateAuth = "DEFERRED"
	result.OrderLifecycle = "DEFERRED"
	result.ConfirmationRequired = true
	result.ConfirmationPhrase = ManualControlConfirmationPhrase
	result.RouteEnabled = false
	result.RiskCheckAvailable = false
	result.RouteBlockers = []string{
		"MANUAL_RISK_CONTRACT_UNAVAILABLE",
		"TRADING_CONTROL_SINK_UNCONFIGURED",
		"PRIVATE_AUTH_DEFERRED",
		"ORDER_LIFECYCLE_DEFERRED",
	}
	result.SafetyNote = "Step 46 establishes the confirmed, hashed, stale-reference-checked manual routing admission contract and durable operator-intent audit. It still cannot publish trading commands or call a venue: PortfolioRisk manual transformation, trading-control sink, private auth and order lifecycle remain deferred."

	snapshot := manualaudit.ReadRecent(p.cfg.ManualAuditDir, 20)
	result.HumanAuditAvailable = snapshot.Available
	result.RoutingContractReady = snapshot.Available
	result.RecentRouteAudits = []ManualRouteAuditRow{}
	if !snapshot.Available {
		result.RouteBlockers = append(result.RouteBlockers, "DURABLE_MANUAL_AUDIT_UNAVAILABLE")
	} else {
		for _, event := range snapshot.Events {
			result.RecentRouteAudits = append(result.RecentRouteAudits, ManualRouteAuditRow{
				RecordedAt: event.RecordedAt, Actor: event.Actor, Result: event.Result, RequestHash: event.RequestHash,
				CorrelationID: event.CorrelationID, ReferenceTargetTimestamp: event.ReferenceTargetTimestamp,
				Submitted: event.Submitted, Blockers: append([]string(nil), event.Blockers...), Detail: event.Detail,
			})
		}
	}
	result.RouteBlockers = uniqueStrings(result.RouteBlockers)
}
func (p *Real) PreviewManualControl(ctx context.Context, req ManualControlPreviewRequest) (ManualControlData, error) {
	checkpoint, err := p.postgres.LatestPipelineCheckpoint(ctx)
	if err != nil {
		return ManualControlData{}, fmt.Errorf("read manual-control reference checkpoint: %w", err)
	}
	window, err := p.marketData.LoadStrategyWindow(ctx, p.cfg.MarketTopN, p.cfg.MarketHistoryDays)
	if err != nil {
		return ManualControlData{}, fmt.Errorf("read manual-control canonical universe: %w", err)
	}

	pairs := make([]string, 0, len(window.Ranking))
	for _, row := range window.Ranking {
		pairs = append(pairs, row.Pair)
	}
	result, requested := buildManualPreviewWithPairs(checkpoint, pairs, req)
	result.Mode = p.runtimeMode()
	result.Exchange = "NORMAL PIPELINE"
	result.SchemaLabel = "asset,weight_pct"
	result.MaxUploadSizeLabel = "256 KB"
	result.ExampleCSV = buildManualExample(checkpoint)
	result.AuditActorLabel = strings.TrimSpace(req.Actor)
	result.BackendAuthoritative = true
	result.SourceMode = "REAL"
	result.CurrentTargetTimestamp = formatTradingTimestamp(checkpoint.Timestamp)
	result.PreviewKind = "TARGET_DELTA_ONLY"
	p.decorateManualControlContract(&result)

	canonical := canonicalManualPortfolio(requested)
	if result.ValidationPassed && canonical != "" {
		digest := sha256.Sum256([]byte(canonical))
		result.RequestHash = "sha256:" + fmt.Sprintf("%x", digest[:])
	}

	// Step 46 additionally validates that every non-CASH target delta has an
	// explicit current execution mapping and a valid public venue-rule row.
	// This is admission evidence only: exact executable quantities remain the
	// responsibility of the normal OrderPlanner pipeline.
	if result.ValidationPassed {
		registry := p.SymbolRegistryStatus(ctx)
		rules := p.VenueTradingRulesStatus(ctx)
		applyManualExecutionConstraints(&result, registry, rules)
	} else {
		result.RouteBlockers = append(result.RouteBlockers, "PREVIEW_VALIDATION_FAILED")
	}
	result.RouteBlockers = uniqueStrings(result.RouteBlockers)
	return result, nil
}
func buildManualPreviewWithPairs(checkpoint pgstore.PipelineCheckpoint, pairs []string, req ManualControlPreviewRequest) (ManualControlData, map[string]float64) {
	result := ManualControlData{PreviewRows: []ManualPreviewRow{}, ValidationIssues: []ManualValidationIssue{}, OrderPreview: []ManualOrderPreviewRow{}, EstimatedFeesLabel: "Not estimated"}
	requested := map[string]float64{}

	csvText := req.CSV
	if len(csvText) > manualControlMaxCSVBytes {
		result.ValidationIssues = append(result.ValidationIssues, ManualValidationIssue{Severity: "ERROR", Field: "file", Message: "CSV exceeds the 256 KB server limit."})
		return result, requested
	}
	if strings.TrimSpace(csvText) == "" {
		result.ValidationIssues = append(result.ValidationIssues, ManualValidationIssue{Severity: "ERROR", Field: "file", Message: "CSV is empty."})
		return result, requested
	}

	reader := csv.NewReader(strings.NewReader(csvText))
	reader.TrimLeadingSpace = true
	reader.FieldsPerRecord = -1
	records, err := reader.ReadAll()
	if err != nil {
		result.ValidationIssues = append(result.ValidationIssues, ManualValidationIssue{Severity: "ERROR", Field: "csv", Message: "CSV parser rejected the file: " + err.Error()})
		return result, requested
	}
	if len(records) < 2 {
		result.ValidationIssues = append(result.ValidationIssues, ManualValidationIssue{Severity: "ERROR", Field: "csv", Message: "CSV must contain a header and at least one target row."})
		return result, requested
	}
	if len(records) > 102 {
		result.ValidationIssues = append(result.ValidationIssues, ManualValidationIssue{Severity: "ERROR", Field: "rows", Message: "CSV contains more than 100 target rows."})
		return result, requested
	}
	if len(records[0]) != 2 || strings.TrimSpace(strings.ToLower(records[0][0])) != "asset" || strings.TrimSpace(strings.ToLower(records[0][1])) != "weight_pct" {
		result.ValidationIssues = append(result.ValidationIssues, ManualValidationIssue{Severity: "ERROR", Field: "schema", Message: "Required header is exactly: asset,weight_pct."})
		return result, requested
	}
	result.ValidationIssues = append(result.ValidationIssues, ManualValidationIssue{Severity: "INFO", Field: "schema", Message: "Required columns asset and weight_pct are present."})

	known := map[string]bool{"CASH": true}
	for _, pair := range pairs {
		known[strings.ToUpper(strings.TrimSpace(pair))] = true
	}
	for asset := range checkpoint.Account.Positions {
		known[strings.ToUpper(strings.TrimSpace(asset))] = true
	}
	for _, strategy := range checkpoint.Decision.Strategies {
		for asset := range strategy.TargetNotionalUSD {
			known[strings.ToUpper(strings.TrimSpace(asset))] = true
		}
	}

	sum := 0.0
	for i, record := range records[1:] {
		line := i + 2
		if len(record) != 2 {
			result.ValidationIssues = append(result.ValidationIssues, ManualValidationIssue{Severity: "ERROR", Field: fmt.Sprintf("row %d", line), Message: "Expected exactly 2 columns."})
			continue
		}
		asset := strings.ToUpper(strings.TrimSpace(record[0]))
		// Exact canonical assets may contain venue/source-native Unicode (for
		// example an explicitly registered Binance symbol). Accept those only
		// when they already exist in the current canonical known set; unknown
		// assets must still satisfy the conservative ASCII identifier pattern.
		if !manualAssetPattern.MatchString(asset) && !known[asset] {
			result.ValidationIssues = append(result.ValidationIssues, ManualValidationIssue{Severity: "ERROR", Field: fmt.Sprintf("row %d asset", line), Message: "Asset must be an exact current canonical asset or contain only A-Z and 0-9 (2-24 chars)."})
			continue
		}
		if _, exists := requested[asset]; exists {
			result.ValidationIssues = append(result.ValidationIssues, ManualValidationIssue{Severity: "ERROR", Field: asset, Message: "Duplicate asset row."})
			continue
		}
		weight, err := strconv.ParseFloat(strings.TrimSpace(record[1]), 64)
		if err != nil || math.IsNaN(weight) || math.IsInf(weight, 0) || weight < 0 || weight > 100 {
			result.ValidationIssues = append(result.ValidationIssues, ManualValidationIssue{Severity: "ERROR", Field: asset, Message: "weight_pct must be a finite number between 0 and 100."})
			continue
		}
		requested[asset] = weight
		sum += weight
		if !known[asset] {
			result.ValidationIssues = append(result.ValidationIssues, ManualValidationIssue{Severity: "ERROR", Field: asset, Message: "Asset is not in the current canonical market universe, current account positions, or current approved target set."})
		}
	}
	if math.Abs(sum-100) > 0.01 {
		result.ValidationIssues = append(result.ValidationIssues, ManualValidationIssue{Severity: "ERROR", Field: "weights", Message: fmt.Sprintf("Requested weights sum to %.4f%%; expected 100.00%% ± 0.01%%.", sum)})
	} else {
		result.ValidationIssues = append(result.ValidationIssues, ManualValidationIssue{Severity: "INFO", Field: "weights", Message: fmt.Sprintf("Requested weights sum to %.2f%%.", sum)})
	}

	if len(checkpoint.Decision.Strategies) != 1 {
		result.ValidationIssues = append(result.ValidationIssues, ManualValidationIssue{Severity: "ERROR", Field: "strategy", Message: "Step 26 manual preview is fail-closed unless exactly one strategy/reference-capital boundary is present."})
	}

	passed := true
	for _, issue := range result.ValidationIssues {
		if issue.Severity == "ERROR" {
			passed = false
			break
		}
	}
	result.ValidationPassed = passed
	if !passed {
		return result, requested
	}

	strategy := checkpoint.Decision.Strategies[0]
	referenceCapital := strategy.ReferenceCapital
	current := map[string]float64{}
	for _, decision := range strategy.Decisions {
		current[strings.ToUpper(decision.Coin)] += decision.TargetWeight * 100
	}
	if len(current) == 0 && referenceCapital > 0 {
		for asset, notional := range strategy.TargetNotionalUSD {
			current[strings.ToUpper(asset)] += notional / referenceCapital * 100
		}
	}
	grossLong := 0.0
	for _, w := range current {
		if w > 0 {
			grossLong += w
		}
	}
	if grossLong <= 100.0001 {
		current["CASH"] = math.Max(0, 100-grossLong)
	}

	assetSet := make(map[string]struct{}, len(requested)+len(current))
	for asset := range requested {
		assetSet[asset] = struct{}{}
	}
	for asset := range current {
		assetSet[asset] = struct{}{}
	}
	assets := make([]string, 0, len(assetSet))
	for asset := range assetSet {
		assets = append(assets, asset)
	}
	sort.Strings(assets)
	turnover := 0.0
	for _, asset := range assets {
		reqWeight := requested[asset]
		curWeight := current[asset]
		delta := reqWeight - curWeight
		turnover += math.Abs(delta)
		notional := "—"
		if referenceCapital > 0 {
			notional = manualSignedUSD(delta / 100 * referenceCapital)
		}
		result.PreviewRows = append(result.PreviewRows, ManualPreviewRow{
			Asset: asset, CurrentWeightPct: curWeight, RequestedWeightPct: reqWeight, ApprovedWeightPct: reqWeight,
			EstimatedNotionalLabel: notional, DeltaPct: delta,
			RiskNote: "NOT RISK-APPROVED · requested target only",
		})
		action := "HOLD"
		if delta > 0.0001 {
			action = "BUY"
		} else if delta < -0.0001 {
			action = "SELL"
		}
		notionalUSD := math.Abs(delta / 100 * referenceCapital)
		result.OrderPreview = append(result.OrderPreview, ManualOrderPreviewRow{
			Asset: asset, Action: action, DeltaWeightPct: delta, EstimatedNotionalLabel: unmanualSignedUSD(delta / 100 * referenceCapital), EstimatedNotionalUSD: notionalUSD,
			EstimatedFeeLabel: "—", Note: "Target-delta preview only; final quantity/precision/min-notional decisions belong to OrderPlanner",
		})
	}
	result.EstimatedTurnoverLabel = fmt.Sprintf("%.2f%% target delta", turnover/2)
	result.ValidationIssues = append(result.ValidationIssues, ManualValidationIssue{Severity: "WARN", Field: "risk", Message: "PortfolioRisk has not evaluated this manual request. Requested weights are not approved targets."})
	result.ValidationIssues = append(result.ValidationIssues, ManualValidationIssue{Severity: "WARN", Field: "routing", Message: "Step 46 routing admission is available, but actual routing remains disabled until PortfolioRisk manual transformation, the trading-control sink, private auth and order lifecycle are proven."})
	return result, requested
}

func applyManualExecutionConstraints(result *ManualControlData, registry SymbolRegistryStatus, rules VenueTradingRulesStatus) {
	if result == nil || !result.ValidationPassed {
		return
	}
	result.RegistryVersion = registry.RegistryVersion
	result.RegistryArtifactSHA256 = registry.RegistryArtifactSHA256
	result.VenueRulesCheckedAt = rules.CheckedAt

	registryByAsset := make(map[string]SymbolRegistryRow, len(registry.Rows))
	for _, row := range registry.Rows {
		registryByAsset[row.Internal] = row
	}
	rulesByAsset := make(map[string]VenueTradingRuleRow, len(rules.Rows))
	for _, row := range rules.Rows {
		rulesByAsset[row.Internal] = row
	}

	valid := true
	if !registry.Validated {
		valid = false
		result.ValidationIssues = append(result.ValidationIssues, ManualValidationIssue{Severity: "ERROR", Field: "execution registry", Message: "The current explicit multi-exchange symbol registry is not validated; manual routing admission fails closed."})
		result.RouteBlockers = append(result.RouteBlockers, "SYMBOL_REGISTRY_NOT_VALIDATED")
	}
	if !rules.Validated {
		valid = false
		result.ValidationIssues = append(result.ValidationIssues, ManualValidationIssue{Severity: "ERROR", Field: "venue rules", Message: "Current public venue trading rules are not validated; manual routing admission fails closed."})
		result.RouteBlockers = append(result.RouteBlockers, "VENUE_RULES_NOT_VALIDATED")
	}

	checked := 0
	for _, order := range result.OrderPreview {
		if order.Asset == "CASH" || order.Action == "HOLD" || math.Abs(order.DeltaWeightPct) <= 0.0001 {
			continue
		}
		checked++
		registryRow, ok := registryByAsset[order.Asset]
		if !ok || registryRow.RuntimeState != "ROUTABLE_PUBLICLY_VALIDATED" || registryRow.RoutingPolicy != "ALLOW_IF_VENUE_PRESENT_AND_RULES_VALID" {
			valid = false
			result.ValidationIssues = append(result.ValidationIssues, ManualValidationIssue{Severity: "ERROR", Field: order.Asset + " execution", Message: "Target delta has no currently routable explicit execution mapping; no heuristic symbol conversion is allowed."})
			result.RouteBlockers = append(result.RouteBlockers, "ASSET_NOT_ROUTABLE:"+order.Asset)
			continue
		}
		rule, ok := rulesByAsset[order.Asset]
		if !ok || rule.State != "VALID" {
			valid = false
			result.ValidationIssues = append(result.ValidationIssues, ManualValidationIssue{Severity: "ERROR", Field: order.Asset + " venue rules", Message: "Target delta has no current validated public venue-rule row."})
			result.RouteBlockers = append(result.RouteBlockers, "VENUE_RULES_BLOCKED:"+order.Asset)
			continue
		}
		if order.EstimatedNotionalUSD > 0 && order.EstimatedNotionalUSD+1e-9 < rule.MinOrderNotionalUSD {
			result.ValidationIssues = append(result.ValidationIssues, ManualValidationIssue{Severity: "WARN", Field: order.Asset + " min notional", Message: fmt.Sprintf("Reference target delta $%.2f is below the current public minimum order notional $%.2f. OrderPlanner must decide the final no-op/aggregation outcome; the dashboard does not fabricate an executable order.", order.EstimatedNotionalUSD, rule.MinOrderNotionalUSD)})
		}
	}
	if checked == 0 {
		result.ValidationIssues = append(result.ValidationIssues, ManualValidationIssue{Severity: "INFO", Field: "execution", Message: "No non-CASH target delta requires an execution mapping in this preview."})
	} else if valid {
		result.ValidationIssues = append(result.ValidationIssues, ManualValidationIssue{Severity: "INFO", Field: "execution", Message: fmt.Sprintf("%d non-CASH target delta(s) have explicit current execution mappings and validated public venue-rule rows. Exact orders remain exclusively owned by OrderPlanner.", checked)})
	}
	result.ExchangeConstraintsValidated = valid
	if !valid {
		result.ValidationPassed = false
	}
	result.RouteBlockers = uniqueStrings(result.RouteBlockers)
}

func uniqueStrings(values []string) []string {
	seen := make(map[string]struct{}, len(values))
	out := make([]string, 0, len(values))
	for _, value := range values {
		value = strings.TrimSpace(value)
		if value == "" {
			continue
		}
		if _, ok := seen[value]; ok {
			continue
		}
		seen[value] = struct{}{}
		out = append(out, value)
	}
	sort.Strings(out)
	return out
}

func canonicalManualPortfolio(requested map[string]float64) string {
	if len(requested) == 0 {
		return ""
	}
	assets := make([]string, 0, len(requested))
	for asset := range requested {
		assets = append(assets, asset)
	}
	sort.Strings(assets)
	var b strings.Builder
	for _, asset := range assets {
		fmt.Fprintf(&b, "%s=%.10f\n", asset, requested[asset])
	}
	return b.String()
}

func buildManualExample(checkpoint pgstore.PipelineCheckpoint) string {
	if len(checkpoint.Decision.Strategies) != 1 {
		return "asset,weight_pct\nCASH,100"
	}
	strategy := checkpoint.Decision.Strategies[0]
	weights := map[string]float64{}
	for _, d := range strategy.Decisions {
		if d.TargetWeight > 0 {
			weights[strings.ToUpper(d.Coin)] += d.TargetWeight * 100
		}
	}
	assets := make([]string, 0, len(weights))
	total := 0.0
	for a, w := range weights {
		assets = append(assets, a)
		total += w
	}
	sort.Strings(assets)
	var b strings.Builder
	b.WriteString("asset,weight_pct\n")
	for _, a := range assets {
		fmt.Fprintf(&b, "%s,%.6g\n", a, weights[a])
	}
	if total < 99.999 {
		fmt.Fprintf(&b, "CASH,%.6g", 100-total)
	}
	return strings.TrimSpace(b.String())
}

func manualSignedUSD(value float64) string {
	if value > 0 {
		return fmt.Sprintf("+$%.2f", value)
	}
	if value < 0 {
		return fmt.Sprintf("-$%.2f", math.Abs(value))
	}
	return "$0.00"
}
func unmanualSignedUSD(value float64) string {
	if math.Abs(value) < 1e-12 {
		return "$0.00"
	}
	return fmt.Sprintf("$%.2f", math.Abs(value))
}
