package provider

import (
	"context"
	"time"
)

// Position history for the replay simulation: fills, fees and funding come from the
// canonical MOCK stream published in state.json (the whole run, not a window).

func simulationTradeEvents(s simulationSnapshot) []tradeEvent {
	fees := map[string]float64{}
	events := make([]tradeEvent, 0, len(s.Fills))
	for index, entry := range s.Accounting {
		switch entry.Type {
		case "TRADING_FEE", "REBATE": // amounts are cash deltas: a fee is negative, a rebate positive
			fees[entry.NativeFillID] -= entry.Amount
		case "FUNDING_PAYMENT":
			events = append(events, tradeEvent{Time: entry.Timestamp, Sequence: uint64(index), Kind: tradeFunding, Asset: entry.Asset, Amount: entry.Amount})
		}
	}
	for index, fill := range s.Fills {
		events = append(events, tradeEvent{
			Time: fill.Timestamp, Sequence: uint64(index), Kind: tradeFill, Asset: fill.Asset,
			Buy: fill.Side == "BUY", Quantity: fill.Quantity, Price: fill.Price, Fee: fees[fill.FillID],
		})
	}
	return events
}

func (p *Simulation) ClosedPositionsReport(_ context.Context) (ClosedPositionsReport, error) {
	s, err := p.load()
	if err != nil {
		return ClosedPositionsReport{}, err
	}
	closed, _ := buildPositionHistory(simulationTradeEvents(s))
	return ClosedPositionsReport{Closed: closed, Complete: true, FundingIn: true, Source: "replay simulation (MOCK venue)", BuiltAt: time.Now().UTC()}, nil
}
