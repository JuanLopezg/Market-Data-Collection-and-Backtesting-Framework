package provider

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"sort"
	"strconv"
	"strings"

	pgstore "control-dashboard-api/internal/integration/postgres"
)

type realRiskData struct {
	ConfigurationFingerprint    string               `json:"configurationFingerprint,omitempty"`
	RiskEvaluations             []riskEvaluationView `json:"riskEvaluations,omitempty"`
	RiskState                   string               `json:"riskState"`
	SourceMode                  string               `json:"sourceMode"`
	SourceUpdatedAt             string               `json:"sourceUpdatedAt"`
	SourceNote                  string               `json:"sourceNote"`
	DecisionTimestamp           string               `json:"decisionTimestamp"`
	DecisionCorrelationID       string               `json:"decisionCorrelationId"`
	DecisionMessageID           string               `json:"decisionMessageId"`
	AccountMessageID            string               `json:"accountMessageId"`
	SignalsMessageID            string               `json:"signalsMessageId"`
	ReferenceCapitalLabel       string               `json:"referenceCapitalLabel"`
	AccountCashLabel            string               `json:"accountCashLabel"`
	ApprovedGrossTargetLabel    string               `json:"approvedGrossTargetLabel"`
	ApprovedNetTargetLabel      string               `json:"approvedNetTargetLabel"`
	ApprovedTargetNotionalLabel string               `json:"approvedTargetNotionalLabel"`
	// Approved gross target over reference capital (0.20 = 0.20x); valid only when known.
	approvedGrossRatio        float64
	approvedGrossRatioKnown   bool
	ActiveTargetCount         int             `json:"activeTargetCount"`
	NonZeroSignalCount        int             `json:"nonZeroSignalCount"`
	StrategyCount             int             `json:"strategyCount"`
	Assets                    []realRiskAsset `json:"realAssets"`
	MissingDiagnostics        []string        `json:"missingDiagnostics"`
	ActiveLimitsAvailable     bool            `json:"activeLimitsAvailable"`
	BreachesAvailable         bool            `json:"breachesAvailable"`
	CurrentValuationAvailable bool            `json:"currentValuationAvailable"`
	EvidenceKind              string          `json:"evidenceKind"`
	DecisionDetail            string          `json:"decisionDetail"`
	Policies                  []riskPolicy    `json:"policies"`
	// Risk page view (risk_view.go); empty when the inputs are not available.
	Summary        []riskCard          `json:"summary,omitempty"`
	History        []riskHistoryPoint  `json:"history,omitempty"`
	StrategyLimits []riskStrategyLimit `json:"strategyLimits,omitempty"`
	Exposures      []riskExposure      `json:"exposures,omitempty"`
}

type realRiskAsset struct {
	Asset               string `json:"asset"`
	Signal              string `json:"signal"`
	ApprovedWeightLabel string `json:"approvedWeightLabel"`
	TargetNotionalLabel string `json:"targetNotionalLabel"`
	StrategyLabel       string `json:"strategyLabel"`
	CurrentWeightLabel  string `json:"currentWeightLabel"`
	LimitLabel          string `json:"limitLabel"`
}

func (p *Real) risk(ctx context.Context) (realRiskData, error) {
	checkpoint, err := p.postgres.LatestPipelineCheckpoint(ctx)
	if errors.Is(err, pgstore.ErrPipelineCheckpointNotPresent) {
		return realRiskData{RiskState: "WAITING FOR DECISION", SourceMode: "REAL", EvidenceKind: "PENDING", DecisionDetail: "No durable PortfolioRisk decision checkpoint exists yet. Risk, exposure and breach status are unavailable, not zero.", SourceNote: "Waiting for same-cycle Strategy/Account input and a persisted Risk output.", AccountCashLabel: "Unavailable", ReferenceCapitalLabel: "Unavailable", ApprovedGrossTargetLabel: "Unavailable", ApprovedNetTargetLabel: "Unavailable", ApprovedTargetNotionalLabel: "Unavailable", Assets: []realRiskAsset{}, Policies: []riskPolicy{}, MissingDiagnostics: []string{"First durable risk decision and its aligned inputs"}}, nil
	}
	if err != nil {
		return realRiskData{}, fmt.Errorf("read durable risk checkpoint: %w", err)
	}
	data := buildRealRisk(checkpoint)
	applyRiskEvaluations(&data, checkpoint.RiskDiagnostics)
	if checkpoint.DecisionConfigChanged {
		data.SourceNote = "The portfolio configuration was changed on purpose after this decision: it was sized with " +
			"the previous configuration, the policies shown are the new one, and the next daily close uses them. " + data.SourceNote
	}

	// Page view: valued account positions, daily PAPER equity (drawdown only; gross is not
	// kept per day) and the persisted risk evaluation. Missing inputs leave sections empty.
	var history []riskEquityPoint
	if p.cfg.PaperComparisonFile != "" {
		for _, row := range readPaperComparison(p.cfg.PaperComparisonFile).Rows {
			history = append(history, riskEquityPoint{label: row.Date, equity: row.LiveEquity})
		}
	}
	if positions, err := p.positions(ctx); err == nil {
		realRiskView(&data, checkpoint.RiskDiagnostics, positions, history)
	}
	return data, nil
}

func realRiskView(data *realRiskData, report *pgstore.RiskDiagnostics, positions realPositionsData, equity []riskEquityPoint) {
	history, maxDrawdown := riskHistory(equity)
	data.History = history

	assetCap := 0.0
	if report != nil {
		totalCapital := 0.0
		for _, strategy := range report.Strategies {
			totalCapital += strategy.ReferenceCapital
		}
		for _, strategy := range report.Strategies {
			share := 1.0
			if totalCapital > 0 {
				share = strategy.ReferenceCapital / totalCapital
			}
			assetCap = math.Max(assetCap, strategy.MaxAssetWeight*share)
			capReduced := false
			for _, asset := range strategy.Assets {
				if asset.SizedWeight != nil && asset.AssetCappedWeight != nil && math.Abs(*asset.AssetCappedWeight) < math.Abs(*asset.SizedWeight)-1e-12 {
					capReduced = true
				}
			}
			data.StrategyLimits = append(data.StrategyLimits, riskStrategyLimit{Name: strategy.Name, Capital: formatDollars(strategy.ReferenceCapital),
				Sizer: strategy.Sizer, MaxAsset: formatPercent(strategy.MaxAssetWeight * 100), MaxGross: formatPercent(strategy.MaxGrossLeverage * 100),
				SizedGross: formatPercent(strategy.GrossAfterAssetCap * 100), Scale: fmt.Sprintf("%.3fx", strategy.GrossScale),
				ApprovedGross: formatPercent(strategy.GrossAfterAssetCap * strategy.GrossScale * 100),
				Binding:       riskBinding(strategy.GrossScale, capReduced)})
		}
	}

	if positions.ValuationState != "AVAILABLE" {
		return
	}
	values := make([]riskPositionValue, 0, len(positions.Positions))
	for _, position := range positions.Positions {
		if !position.ValuationAvailable {
			continue
		}
		row := riskPositionValue{asset: position.Asset, value: position.EffectiveQty * position.CurrentPrice}
		if position.TargetAvailable {
			weight := position.TargetWeightPct / 100
			row.approved = &weight
		}
		values = append(values, row)
	}
	data.Summary, data.Exposures = riskAccountView(positions.equityValue, values, assetCap, history, maxDrawdown)
}

func buildRealRisk(checkpoint pgstore.PipelineCheckpoint) realRiskData {
	totalReferenceCapital := 0.0
	totalGrossTarget := 0.0
	totalNetTarget := 0.0
	targetsByAsset := map[string]float64{}
	strategyByAsset := map[string][]uint64{}
	approvedWeightsByAsset := map[string]float64{}

	for _, strategy := range checkpoint.Decision.Strategies {
		totalReferenceCapital += strategy.ReferenceCapital
		for asset, notional := range strategy.TargetNotionalUSD {
			if strings.TrimSpace(asset) == "" {
				continue
			}
			targetsByAsset[asset] += notional
			totalGrossTarget += math.Abs(notional)
			totalNetTarget += notional
			strategyByAsset[asset] = appendUniqueUint64(strategyByAsset[asset], strategy.StrategyID)
		}
		for _, decision := range strategy.Decisions {
			if strings.TrimSpace(decision.Coin) == "" {
				continue
			}
			approvedWeightsByAsset[decision.Coin] += decision.TargetWeight
			strategyByAsset[decision.Coin] = appendUniqueUint64(strategyByAsset[decision.Coin], strategy.StrategyID)
		}
	}

	signalTotals := map[string]float64{}
	nonZeroSignalCount := 0
	for _, strategy := range checkpoint.Signals.Strategies {
		for asset, signal := range strategy.Signals {
			signalTotals[asset] += signal
			if math.Abs(signal) > 1e-12 {
				nonZeroSignalCount++
			}
		}
	}

	assetsSet := map[string]struct{}{}
	for asset := range targetsByAsset {
		assetsSet[asset] = struct{}{}
	}
	for asset := range approvedWeightsByAsset {
		assetsSet[asset] = struct{}{}
	}
	for asset := range signalTotals {
		assetsSet[asset] = struct{}{}
	}
	for asset := range checkpoint.Account.Positions {
		assetsSet[asset] = struct{}{}
	}

	assets := make([]string, 0, len(assetsSet))
	for asset := range assetsSet {
		if strings.TrimSpace(asset) != "" {
			assets = append(assets, asset)
		}
	}
	sort.Strings(assets)

	policies := checkpointRiskPolicies(checkpoint.PortfolioConfig)
	rows := make([]realRiskAsset, 0, len(assets))
	activeTargets := 0
	for _, asset := range assets {
		notional := targetsByAsset[asset]
		if math.Abs(notional) > 1e-12 {
			activeTargets++
		}
		weight := approvedWeightsByAsset[asset]
		// Per-strategy percentages have different denominators. Display the combined
		// approved notional divided by combined emitted reference capital instead.
		if totalReferenceCapital > 0 {
			weight = notional / totalReferenceCapital
		}
		weightLabel, targetLabel := formatPercent(weight*100), formatUSD(notional)
		if _, emitted := targetsByAsset[asset]; !emitted {
			weightLabel, targetLabel = "Not emitted", "HOLD / no new target"
		}
		signalLabel := "UNKNOWN"
		if value, present := signalTotals[asset]; present {
			signalLabel = riskSignalLabel(value)
		}
		rows = append(rows, realRiskAsset{
			Asset:               asset,
			Signal:              signalLabel,
			ApprovedWeightLabel: weightLabel,
			TargetNotionalLabel: targetLabel,
			StrategyLabel:       formatStrategyIDs(strategyByAsset[asset]),
			CurrentWeightLabel:  "Not valued",
			LimitLabel:          "Active config not wired",
		})
		if len(policies) > 0 {
			rows[len(rows)-1].LimitLabel = "Per-strategy policy below"
		}
	}
	detail := "Persisted approved instructions; omitted assets preserve holdings and are not zero targets."
	state := "APPROVED DECISION OBSERVED"
	capitalLabel, notionalLabel := formatUSD(totalReferenceCapital), formatUSD(totalGrossTarget)
	if len(checkpoint.Decision.Strategies) == 0 {
		capitalLabel, notionalLabel = "Not emitted", "No new target"
		state = "NO NEW TARGETS"
		detail = "PortfolioRisk processed this cycle and emitted no new targets. HOLD decisions or insufficient sizing history can produce this result; it does not mean zero exposure or missing processing."
	}

	grossPct := "—"
	netPct := "—"
	grossRatio := 0.0
	if totalReferenceCapital > 0 {
		grossRatio = totalGrossTarget / totalReferenceCapital
		grossPct = formatPercent((totalGrossTarget / totalReferenceCapital) * 100)
		netPct = formatSignedPercent((totalNetTarget / totalReferenceCapital) * 100)
	}

	return realRiskData{
		RiskState:                   state,
		EvidenceKind:                "DURABLE_DECISION",
		DecisionDetail:              detail,
		Policies:                    policies,
		SourceMode:                  "REAL",
		SourceUpdatedAt:             formatTradingTimestamp(checkpoint.Timestamp),
		SourceNote:                  "Decision lineage is persisted in portfolio_risk_live_decision_checkpoint. Policy values come from the service's persisted configuration identity; they are settings, not a breach evaluation. Raw sizing, binding constraints and volatility transforms remain unavailable.",
		DecisionTimestamp:           formatTradingTimestamp(checkpoint.Timestamp),
		DecisionCorrelationID:       checkpoint.Decision.Metadata.CorrelationID,
		DecisionMessageID:           checkpoint.Decision.Metadata.MessageID,
		AccountMessageID:            checkpoint.Account.Metadata.MessageID,
		SignalsMessageID:            checkpoint.Signals.Metadata.MessageID,
		ReferenceCapitalLabel:       capitalLabel,
		AccountCashLabel:            formatUSD(checkpoint.Account.Cash),
		ApprovedGrossTargetLabel:    grossPct,
		ApprovedNetTargetLabel:      netPct,
		ApprovedTargetNotionalLabel: notionalLabel,
		approvedGrossRatio:          grossRatio,
		approvedGrossRatioKnown:     totalReferenceCapital > 0,
		ActiveTargetCount:           activeTargets,
		NonZeroSignalCount:          nonZeroSignalCount,
		StrategyCount:               len(checkpoint.Signals.Strategies),
		Assets:                      rows,
		MissingDiagnostics: []string{
			"Per-cycle risk configuration fingerprint and evaluated constraint report",
			"Raw pre-risk target before PortfolioRisk transformations",
			"Per-asset binding constraint / clipping reason",
			"Volatility scaling factor and covariance diagnostics",
			"Current marked portfolio weights and PnL",
			"Durable breach/warning report and kill-switch/readiness state",
		},
		ActiveLimitsAvailable:     len(policies) > 0,
		BreachesAvailable:         false,
		CurrentValuationAvailable: false,
	}
}

// These are persisted configuration values, not reconstructed clipping/breach decisions.
type riskPolicy struct {
	StrategyID      uint64 `json:"strategyId"`
	Name            string `json:"name"`
	AllocationLabel string `json:"allocationLabel"`
	Sizer           string `json:"sizer"`
	GrossLimitLabel string `json:"grossLimitLabel"`
	AssetLimitLabel string `json:"assetLimitLabel"`
	Rebalance       string `json:"rebalance"`
}

func checkpointRiskPolicies(identity string) []riskPolicy {
	rows := []riskPolicy{}
	var config struct {
		Strategies []struct {
			ID         uint64   `json:"id"`
			Name       string   `json:"name"`
			Allocation *float64 `json:"allocation_weight"`
			Sizer      struct {
				Type   string   `json:"type"`
				Weight *float64 `json:"weight_per_full_signal"`
			} `json:"sizer"`
			Risk struct {
				Gross *float64 `json:"max_gross_leverage"`
				Asset *float64 `json:"max_asset_weight"`
			} `json:"risk"`
			Rebalance struct {
				Type string `json:"type"`
			} `json:"rebalance"`
		} `json:"strategies"`
	}
	// Service identity appends mode/history settings after the JSON document.
	if json.NewDecoder(strings.NewReader(identity)).Decode(&config) != nil {
		return rows
	}
	for _, strategy := range config.Strategies {
		if strategy.ID == 0 || strategy.Allocation == nil || strategy.Risk.Gross == nil || strategy.Risk.Asset == nil {
			return []riskPolicy{}
		}
		if *strategy.Allocation < 0 || *strategy.Risk.Gross < 0 || *strategy.Risk.Asset < 0 {
			return []riskPolicy{}
		}
		sizer := strategy.Sizer.Type
		if strategy.Sizer.Weight != nil {
			sizer += " · " + formatPercent(*strategy.Sizer.Weight*100) + " per full signal"
		}
		rows = append(rows, riskPolicy{strategy.ID, strategy.Name, formatPercent(*strategy.Allocation * 100), sizer,
			formatPercent(*strategy.Risk.Gross * 100), formatPercent(*strategy.Risk.Asset * 100), strategy.Rebalance.Type})
	}
	return rows
}

func riskSignalLabel(value float64) string {
	if value > 1e-12 {
		return "LONG"
	}
	if value < -1e-12 {
		return "SHORT"
	}
	return "FLAT"
}

func formatPercent(value float64) string {
	if math.IsNaN(value) || math.IsInf(value, 0) {
		return "—"
	}
	return fmt.Sprintf("%.1f%%", value)
}

func formatSignedPercent(value float64) string {
	if math.IsNaN(value) || math.IsInf(value, 0) {
		return "—"
	}
	if value > 0 {
		return fmt.Sprintf("+%.1f%%", value)
	}
	return fmt.Sprintf("%.1f%%", value)
}

func formatStrategyIDs(ids []uint64) string {
	if len(ids) == 0 {
		return "—"
	}
	sort.Slice(ids, func(i, j int) bool { return ids[i] < ids[j] })
	parts := make([]string, 0, len(ids))
	for _, id := range ids {
		parts = append(parts, strconv.FormatUint(id, 10))
	}
	return strings.Join(parts, ", ")
}

func appendUniqueUint64(values []uint64, value uint64) []uint64 {
	for _, existing := range values {
		if existing == value {
			return values
		}
	}
	return append(values, value)
}
