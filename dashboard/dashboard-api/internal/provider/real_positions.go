package provider

import (
	"context"
	"fmt"
	"math"
	"sort"
	"strconv"
	"strings"
	"time"

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
	// Position history from the persisted fills (see position_history.go).
	ClosedPositions  []closedPositionRow `json:"closedPositions"`
	ClosedAvailable  bool                `json:"closedAvailable"`  // whole fill history read
	FundingAvailable bool                `json:"fundingAvailable"` // funding payments recorded
	// Kraken LIVE: "Kraken mark price", or "Binance daily close" when Kraken cannot be read.
	PriceSource string `json:"priceSource,omitempty"`
	// Numeric valuation (set with ValuationState AVAILABLE) for other read models. In Kraken
	// LIVE equityValue includes collateralValue (BTC collateral after haircut).
	equityValue, grossValue, collateralValue float64
	// Equity without unrealized PnL (cash + entry value + collateral); known when every
	// position has an entry price.
	balanceValue float64
	balanceKnown bool
}

type realPosition struct {
	StopLoss                *positionStopLoss      `json:"stopLoss,omitempty"`
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
	OpenedAt                string                 `json:"openedAt"`
	FeesUSD                 float64                `json:"feesUsd"`    // fees paid by this position so far
	FundingUSD              float64                `json:"fundingUsd"` // funding so far (+ received)
	NetPnLUSD               float64                `json:"netPnlUsd"`  // unrealized + realized so far - fees + funding
	FeesAvailable           bool                   `json:"feesAvailable"`
	FundingAvailable        bool                   `json:"fundingAvailable"`
	// Kraken LIVE: the market's current funding rate (+ = longs pay), per hour and annualized.
	FundingRateHourlyPct float64 `json:"fundingRateHourlyPct"`
	FundingRateAnnualPct float64 `json:"fundingRateAnnualPct"`
	FundingRateAvailable bool    `json:"fundingRateAvailable"`
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
	data := buildRealPositions(state)
	// Only a real venue pays funding; the runtime books it on the position it was paid on.
	data.FundingAvailable = p.krakenLive()
	p.valuePositions(ctx, &data, state.Snapshot.AccountCash)
	// The runtime's position ledger is the source; rebuilding from the fills is only the
	// fallback for an ExecutionState that does not write it yet.
	if accounting, err := p.postgres.Accounting(ctx); err == nil && accounting.Available {
		closed, open := lotsFromAccounting(accounting)
		applyPositionLots(&data, closed, open)
	} else if window, err := p.postgres.TradeHistoryFills(ctx); err == nil && window.TotalRows <= int64(len(window.Rows)) {
		data.FundingAvailable = false // fills carry no funding
		applyRealPositionHistory(&data, realTradeEvents(window.Rows))
	}
	applyPositionStops(&data, state.Snapshot.Orders)
	return data, nil
}

// lotsFromAccounting converts the runtime position tables into the dashboard's lots.
func lotsFromAccounting(accounting pgstore.Accounting) ([]positionLot, map[string]*positionLot) {
	convert := func(row pgstore.AccountingPosition) positionLot {
		return positionLot{
			Asset: row.Coin, Side: row.Side, OpenedAt: row.OpenedAt, ClosedAt: row.ClosedAt,
			Quantity: row.Quantity, MaxQuantity: row.MaxQuantity, EntryPrice: row.EntryPrice,
			EntryValue: row.EntryValue, ExitValue: row.ExitValue, ExitQty: row.ExitQuantity,
			GrossPnL: row.GrossPnL, Fees: row.Fees, Funding: row.Funding,
		}
	}
	closed := make([]positionLot, 0, len(accounting.Closed))
	for _, row := range accounting.Closed {
		closed = append(closed, convert(row))
	}
	open := make(map[string]*positionLot, len(accounting.Open))
	for _, row := range accounting.Open {
		lot := convert(row)
		open[row.Coin] = &lot
	}
	return closed, open
}

// Fallback when ExecutionState has no position ledger: fills carry their commission but no
// funding, so funding stays out of this history.
func realTradeEvents(fills []tradingwire.Fill) []tradeEvent {
	events := make([]tradeEvent, 0, len(fills))
	for _, fill := range fills {
		events = append(events, tradeEvent{
			Time: fill.Timestamp, Sequence: fill.FillID, Kind: tradeFill, Asset: fill.Coin,
			Buy: fill.Side == 0, Quantity: fill.Quantity, Price: fill.Price, Fee: fill.Commission,
		})
	}
	return events
}

func applyRealPositionHistory(data *realPositionsData, events []tradeEvent) {
	closed, lots := buildPositionHistory(events)
	applyPositionLots(data, closed, lots)
}

func applyPositionLots(data *realPositionsData, closed []positionLot, lots map[string]*positionLot) {
	data.ClosedPositions = closedPositionRows(closed)
	data.ClosedAvailable = true
	for index := range data.Positions {
		position := &data.Positions[index]
		lot := lots[position.Asset]
		if lot == nil {
			continue
		}
		position.OpenedAt = tradeTimeLabel(lot.OpenedAt)
		position.FeesUSD = lot.Fees
		position.FeesAvailable = true
		position.FundingUSD = lot.Funding
		position.FundingAvailable = data.FundingAvailable
		if position.PnLAvailable {
			position.NetPnLUSD = position.PnLUSD + lot.GrossPnL - lot.Fees + lot.Funding
		}
	}
}

func (p *Real) ClosedPositionsReport(ctx context.Context) (ClosedPositionsReport, error) {
	if accounting, err := p.postgres.Accounting(ctx); err == nil && accounting.Available {
		closed, _ := lotsFromAccounting(accounting)
		return ClosedPositionsReport{Closed: closed, Complete: true,
			Source: "runtime position ledger (trading_closed_positions; funding only from a real venue)", BuiltAt: time.Now().UTC()}, nil
	}
	window, err := p.postgres.TradeHistoryFills(ctx)
	if err != nil {
		return ClosedPositionsReport{}, fmt.Errorf("read trade history: %w", err)
	}
	closed, _ := buildPositionHistory(realTradeEvents(window.Rows))
	return ClosedPositionsReport{Closed: closed, Complete: window.TotalRows <= int64(len(window.Rows)),
		Source: "trading_fills (funding not recorded)", BuiltAt: time.Now().UTC()}, nil
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
		ClosedPositions:  []closedPositionRow{},
		AccountCash:      formatUSD(state.Snapshot.AccountCash),
		SourceMode:       "REAL",
		SourceUpdatedAt:  state.UpdatedAt,
		ValuationState:   "UNAVAILABLE",
		SourceNote:       "Quantities from trading_runtime_state.snapshot; marks are the latest completed closes; targets from the latest PortfolioRisk decision; entry/PnL from the average cost of persisted fills.",
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
	if value == 0 {
		return "0"
	}
	if value > 0 {
		return "+" + formatQuantity(value)
	}
	return "-" + formatQuantity(math.Abs(value))
}

func formatUSD(value float64) string {
	return fmt.Sprintf("$%.2f", value)
}
