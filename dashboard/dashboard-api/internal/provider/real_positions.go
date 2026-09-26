package provider

import (
	"context"
	"fmt"
	"math"
	"sort"
	"strconv"
	"strings"

	pgstore "control-dashboard-api/internal/integration/postgres"
	"control-dashboard-api/internal/tradingwire"
)

type realPositionsData struct {
	TotalEquity      string         `json:"totalEquity"`
	ActivePositions  int            `json:"activePositions"`
	GrossExposure    string         `json:"grossExposure"`
	GrossExposurePct string         `json:"grossExposurePct"`
	NetExposure      string         `json:"netExposure"`
	UnrealizedPnL    string         `json:"unrealizedPnl"`
	Positions        []realPosition `json:"positions"`
	AccountCash      string         `json:"accountCash"`
	SourceMode       string         `json:"sourceMode"`
	SourceUpdatedAt  string         `json:"sourceUpdatedAt"`
	ValuationState   string         `json:"valuationState"`
	SourceNote       string         `json:"sourceNote"`
}

type realPosition struct {
	Asset                   string                 `json:"asset"`
	Side                    string                 `json:"side"`
	Quantity                float64                `json:"quantity"`
	QuantityLabel           string                 `json:"quantityLabel"`
	EntryPrice              float64                `json:"entryPrice"`
	EntryPriceLabel         string                 `json:"entryPriceLabel"`
	CurrentPrice            float64                `json:"currentPrice"`
	CurrentPriceLabel       string                 `json:"currentPriceLabel"`
	PnLUSD                  float64                `json:"pnlUsd"`
	PnLUSDLabel             string                 `json:"pnlUsdLabel"`
	PnLPct                  float64                `json:"pnlPct"`
	PnLPctLabel             string                 `json:"pnlPctLabel"`
	CurrentWeightPct        float64                `json:"currentWeightPct"`
	TargetWeightPct         float64                `json:"targetWeightPct"`
	TargetQty               float64                `json:"targetQty"`
	EffectiveQty            float64                `json:"effectiveQty"`
	LocalQty                float64                `json:"localQty"`
	ExchangeQty             float64                `json:"exchangeQty"`
	DeltaQty                float64                `json:"deltaQty"`
	Status                  string                 `json:"status"`
	ValuationAvailable      bool                   `json:"valuationAvailable"`
	TargetAvailable         bool                   `json:"targetAvailable"`
	PnLAvailable            bool                   `json:"pnlAvailable"`
	ReconciliationAvailable bool                   `json:"reconciliationAvailable"`
	StrategyBreakdown       []realStrategyPosition `json:"strategyBreakdown"`
}

type realStrategyPosition struct {
	StrategyID    uint64  `json:"strategyId"`
	Quantity      float64 `json:"quantity"`
	QuantityLabel string  `json:"quantityLabel"`
}

func (p *Real) positions(ctx context.Context) (realPositionsData, error) {
	state, err := p.postgres.RuntimeState(ctx)
	if err != nil {
		return realPositionsData{}, fmt.Errorf("read durable positions: %w", err)
	}
	return buildRealPositions(state), nil
}

func buildRealPositions(state pgstore.RuntimeState) realPositionsData {
	positions := make([]realPosition, 0, len(state.Snapshot.AccountPositions))
	coins := make([]string, 0, len(state.Snapshot.AccountPositions))
	for coin, quantity := range state.Snapshot.AccountPositions {
		if math.Abs(quantity) <= 1e-12 {
			continue
		}
		coins = append(coins, coin)
	}
	sort.Strings(coins)

	for _, coin := range coins {
		physicalQty := state.Snapshot.AccountPositions[coin]
		side := "Long"
		if physicalQty < 0 {
			side = "Short"
		}
		positions = append(positions, realPosition{
			Asset:                   coin,
			Side:                    side,
			Quantity:                math.Abs(physicalQty),
			QuantityLabel:           formatQuantity(math.Abs(physicalQty)),
			EntryPriceLabel:         "—",
			CurrentPriceLabel:       "—",
			PnLUSDLabel:             "—",
			PnLPctLabel:             "—",
			EffectiveQty:            physicalQty,
			LocalQty:                physicalQty,
			Status:                  "UNKNOWN",
			ValuationAvailable:      false,
			TargetAvailable:         false,
			PnLAvailable:            false,
			ReconciliationAvailable: false,
			StrategyBreakdown:       strategyBreakdown(state.Snapshot.Strategies, coin),
		})
	}

	return realPositionsData{
		TotalEquity:      "Not valued",
		ActivePositions:  len(positions),
		GrossExposure:    "Not valued",
		GrossExposurePct: "—",
		NetExposure:      "Not valued",
		UnrealizedPnL:    "Not available",
		Positions:        positions,
		AccountCash:      formatUSD(state.Snapshot.AccountCash),
		SourceMode:       "REAL",
		SourceUpdatedAt:  state.UpdatedAt,
		ValuationState:   "UNAVAILABLE",
		SourceNote:       "Physical quantities and strategy virtual positions come from trading_runtime_state.snapshot. Price valuation, approved targets, PnL and exchange reconciliation are intentionally not inferred in Step 17.",
	}
}

func strategyBreakdown(strategies []tradingwire.StrategyStateSnapshot, coin string) []realStrategyPosition {
	result := make([]realStrategyPosition, 0)
	for _, strategy := range strategies {
		quantity, ok := strategy.VirtualPositions[coin]
		if !ok || math.Abs(quantity) <= 1e-12 {
			continue
		}
		result = append(result, realStrategyPosition{
			StrategyID:    strategy.StrategyID,
			Quantity:      quantity,
			QuantityLabel: signedQuantity(quantity),
		})
	}
	sort.Slice(result, func(i, j int) bool { return result[i].StrategyID < result[j].StrategyID })
	return result
}

func formatQuantity(value float64) string {
	text := strconv.FormatFloat(value, 'f', 8, 64)
	text = strings.TrimRight(strings.TrimRight(text, "0"), ".")
	if text == "" {
		return "0"
	}
	return text
}

func signedQuantity(value float64) string {
	if value > 0 {
		return "+" + formatQuantity(value)
	}
	return "-" + formatQuantity(math.Abs(value))
}

func formatUSD(value float64) string {
	return fmt.Sprintf("$%.2f", value)
}
