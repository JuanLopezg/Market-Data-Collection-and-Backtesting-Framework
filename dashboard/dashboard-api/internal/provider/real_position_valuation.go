package provider

import (
	"context"
	"fmt"
	"math"
	"sort"

	"control-dashboard-api/internal/tradingwire"
)

// Position valuation for the PAPER (real) provider.
//
// Marks are the latest completed daily closes in the market SQLite; equity is account cash
// plus marked positions (the PAPER account pays for positions from cash). Target weights
// come from the latest durable PortfolioRisk decision. Entry price and unrealized PnL use
// the average cost of the persisted fills, and only when the whole fill history fits the
// bounded read. Anything that cannot be established stays unavailable ("—"), never zero.

const valuationFillWindow = 1000

type positionValuationInputs struct {
	cash    float64
	marks   map[string]float64 // latest close per asset
	targets map[string]float64 // approved target notional per asset (USD); nil = unknown
	entries map[string]float64 // average entry price per asset; nil = unknown
	status  string             // per-position alignment derived from reconciliation
}

func (p *Real) valuePositions(ctx context.Context, data *realPositionsData, cash float64) {
	inputs := positionValuationInputs{cash: cash, status: "PENDING"}

	assets := make([]string, 0, len(data.Positions))
	for _, position := range data.Positions {
		assets = append(assets, position.Asset)
	}
	if p.marketData != nil {
		if closes, err := p.marketData.LatestCloses(ctx, assets); err == nil {
			inputs.marks = map[string]float64{}
			for asset, row := range closes {
				inputs.marks[asset] = row.Close
			}
		}
	}
	if checkpoint, err := p.postgres.LatestPipelineCheckpoint(ctx); err == nil {
		inputs.targets = map[string]float64{}
		for _, strategy := range checkpoint.Decision.Strategies {
			for asset, usd := range strategy.TargetNotionalUSD {
				inputs.targets[asset] += usd
			}
		}
	}
	if window, err := p.postgres.LedgerFills(ctx, valuationFillWindow); err == nil && window.TotalRows <= int64(len(window.Rows)) {
		inputs.entries = averageEntryPrices(window.Rows)
	}
	if reconciliation, err := p.reconciliation(ctx); err == nil {
		inputs.status = overviewReconciliationCheckState(reconciliation.Status)
	}
	applyPositionValuation(data, inputs)
}

// averageEntryPrices replays fills in time order: adding to a position averages its cost,
// reducing keeps it, crossing zero restarts it at the fill price.
func averageEntryPrices(fills []tradingwire.Fill) map[string]float64 {
	ordered := append([]tradingwire.Fill(nil), fills...)
	sort.Slice(ordered, func(i, j int) bool {
		if ordered[i].Timestamp != ordered[j].Timestamp {
			return ordered[i].Timestamp < ordered[j].Timestamp
		}
		return ordered[i].FillID < ordered[j].FillID
	})
	quantity, entry := map[string]float64{}, map[string]float64{}
	for _, fill := range ordered {
		delta := fill.Quantity
		if fill.Side == 1 {
			delta = -fill.Quantity
		}
		current := quantity[fill.Coin]
		switch {
		case current == 0 || (current > 0) == (delta > 0):
			entry[fill.Coin] = (math.Abs(current)*entry[fill.Coin] + math.Abs(delta)*fill.Price) / (math.Abs(current) + math.Abs(delta))
		case math.Abs(delta) > math.Abs(current):
			entry[fill.Coin] = fill.Price
		}
		quantity[fill.Coin] = current + delta
		if math.Abs(quantity[fill.Coin]) <= 1e-12 {
			quantity[fill.Coin], entry[fill.Coin] = 0, 0
		}
	}
	return entry
}

func applyPositionValuation(data *realPositionsData, inputs positionValuationInputs) {
	equity, gross, net, unrealized := inputs.cash, 0.0, 0.0, 0.0
	valued := len(data.Positions) == 0 || inputs.marks != nil
	for _, position := range data.Positions {
		mark, ok := inputs.marks[position.Asset]
		if !ok {
			valued = false
			continue
		}
		notional := position.EffectiveQty * mark
		equity += notional
		gross += math.Abs(notional)
		net += notional
	}

	pnlComplete := true
	for index := range data.Positions {
		position := &data.Positions[index]
		position.Status = inputs.status
		mark, ok := inputs.marks[position.Asset]
		if !ok || !valued || equity <= 0 {
			continue
		}
		position.CurrentPrice = mark
		position.CurrentPriceLabel = formatPriceLabel(mark)
		position.CurrentWeightPct = position.EffectiveQty * mark / equity * 100
		position.ValuationAvailable = true
		if inputs.targets != nil {
			target := inputs.targets[position.Asset]
			position.TargetWeightPct = target / equity * 100
			position.TargetQty = target / mark
			position.TargetAvailable = true
		}
		if entry, ok := inputs.entries[position.Asset]; ok && entry > 0 {
			pnl := position.EffectiveQty * (mark - entry)
			position.EntryPrice, position.EntryPriceLabel = entry, formatPriceLabel(entry)
			position.PnLUSD, position.PnLUSDLabel = pnl, formatSignedUSDLabel(pnl)
			position.PnLPct = pnl / math.Abs(position.EffectiveQty*entry) * 100
			position.PnLPctLabel = fmt.Sprintf("%+.2f%%", position.PnLPct)
			position.PnLAvailable = true
			unrealized += pnl
		} else {
			pnlComplete = false
		}
	}

	if !valued || equity <= 0 {
		return
	}
	data.TotalEquity = formatUSD(equity)
	data.GrossExposure = formatUSD(gross)
	data.GrossExposurePct = fmt.Sprintf("%.2f%%", gross/equity*100)
	data.NetExposure = formatSignedUSDLabel(net)
	data.ValuationState = "AVAILABLE"
	data.equityValue, data.grossValue = equity, gross
	if pnlComplete {
		data.UnrealizedPnL = formatSignedUSDLabel(unrealized)
	}
}

func formatPriceLabel(value float64) string {
	if value >= 1 {
		return fmt.Sprintf("$%.4f", value)
	}
	return fmt.Sprintf("$%.8f", value)
}

func formatSignedUSDLabel(value float64) string {
	if value < 0 {
		return fmt.Sprintf("-$%.2f", -value)
	}
	return fmt.Sprintf("+$%.2f", value)
}
