package provider

import (
	"context"
	"fmt"
	"math"
	"sort"
	"strconv"
	"strings"

	pgstore "control-dashboard-api/internal/integration/postgres"
)

type realRiskData struct {
	RiskState                   string          `json:"riskState"`
	SourceMode                  string          `json:"sourceMode"`
	SourceUpdatedAt             string          `json:"sourceUpdatedAt"`
	SourceNote                  string          `json:"sourceNote"`
	DecisionTimestamp           string          `json:"decisionTimestamp"`
	DecisionCorrelationID       string          `json:"decisionCorrelationId"`
	DecisionMessageID           string          `json:"decisionMessageId"`
	AccountMessageID            string          `json:"accountMessageId"`
	SignalsMessageID            string          `json:"signalsMessageId"`
	ReferenceCapitalLabel       string          `json:"referenceCapitalLabel"`
	AccountCashLabel            string          `json:"accountCashLabel"`
	ApprovedGrossTargetLabel    string          `json:"approvedGrossTargetLabel"`
	ApprovedNetTargetLabel      string          `json:"approvedNetTargetLabel"`
	ApprovedTargetNotionalLabel string          `json:"approvedTargetNotionalLabel"`
	ActiveTargetCount           int             `json:"activeTargetCount"`
	NonZeroSignalCount          int             `json:"nonZeroSignalCount"`
	StrategyCount               int             `json:"strategyCount"`
	Assets                      []realRiskAsset `json:"realAssets"`
	MissingDiagnostics          []string        `json:"missingDiagnostics"`
	ActiveLimitsAvailable       bool            `json:"activeLimitsAvailable"`
	BreachesAvailable           bool            `json:"breachesAvailable"`
	CurrentValuationAvailable   bool            `json:"currentValuationAvailable"`
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
	if err != nil {
		return realRiskData{}, fmt.Errorf("read durable risk checkpoint: %w", err)
	}
	return buildRealRisk(checkpoint), nil
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

	assets := make([]string, 0, len(assetsSet))
	for asset := range assetsSet {
		if strings.TrimSpace(asset) != "" {
			assets = append(assets, asset)
		}
	}
	sort.Strings(assets)

	rows := make([]realRiskAsset, 0, len(assets))
	activeTargets := 0
	for _, asset := range assets {
		notional := targetsByAsset[asset]
		if math.Abs(notional) > 1e-12 {
			activeTargets++
		}
		weight := approvedWeightsByAsset[asset]
		// DecisionBatch is authoritative for approved weight. If a producer omits
		// decisions[] but keeps target_notional_usd, derive the display weight only
		// from the same approved boundary/reference capital and label no policy limit.
		if math.Abs(weight) <= 1e-12 && totalReferenceCapital > 0 {
			weight = notional / totalReferenceCapital
		}
		rows = append(rows, realRiskAsset{
			Asset:               asset,
			Signal:              riskSignalLabel(signalTotals[asset]),
			ApprovedWeightLabel: formatPercent(weight * 100),
			TargetNotionalLabel: formatUSD(notional),
			StrategyLabel:       formatStrategyIDs(strategyByAsset[asset]),
			CurrentWeightLabel:  "Not valued",
			LimitLabel:          "Active config not wired",
		})
	}

	grossPct := "—"
	netPct := "—"
	if totalReferenceCapital > 0 {
		grossPct = formatPercent((totalGrossTarget / totalReferenceCapital) * 100)
		netPct = formatSignedPercent((totalNetTarget / totalReferenceCapital) * 100)
	}

	return realRiskData{
		RiskState:                   "APPROVED DECISION OBSERVED",
		SourceMode:                  "REAL",
		SourceUpdatedAt:             formatTradingTimestamp(checkpoint.Timestamp),
		SourceNote:                  "Canonical approved targets come from portfolio_risk_live_decision_checkpoint. The checkpoint persists the exact StrategyIntentBatch, AccountSnapshot and approved DecisionBatch. Active policy limits, raw pre-risk sizing, binding-rule diagnostics, volatility scaling and breach evaluation are not persisted here and are not inferred by the dashboard.",
		DecisionTimestamp:           formatTradingTimestamp(checkpoint.Timestamp),
		DecisionCorrelationID:       checkpoint.Decision.Metadata.CorrelationID,
		DecisionMessageID:           checkpoint.Decision.Metadata.MessageID,
		AccountMessageID:            checkpoint.Account.Metadata.MessageID,
		SignalsMessageID:            checkpoint.Signals.Metadata.MessageID,
		ReferenceCapitalLabel:       formatUSD(totalReferenceCapital),
		AccountCashLabel:            formatUSD(checkpoint.Account.Cash),
		ApprovedGrossTargetLabel:    grossPct,
		ApprovedNetTargetLabel:      netPct,
		ApprovedTargetNotionalLabel: formatUSD(totalGrossTarget),
		ActiveTargetCount:           activeTargets,
		NonZeroSignalCount:          nonZeroSignalCount,
		StrategyCount:               len(checkpoint.Decision.Strategies),
		Assets:                      rows,
		MissingDiagnostics: []string{
			"Active portfolio/risk configuration version and runtime limit values",
			"Raw pre-risk target before PortfolioRisk transformations",
			"Per-asset binding constraint / clipping reason",
			"Volatility scaling factor and covariance diagnostics",
			"Current marked portfolio weights and PnL",
			"Durable breach/warning report and kill-switch/readiness state",
		},
		ActiveLimitsAvailable:     false,
		BreachesAvailable:         false,
		CurrentValuationAvailable: false,
	}
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
