package provider

import (
	"context"
	"fmt"
	"strings"
	"sync"
	"time"

	krakenpublic "control-dashboard-api/internal/integration/kraken"
	"control-dashboard-api/internal/integration/symbolmap"
)

// Kraken LIVE marks: the dashboard values open positions at Kraken's current mark price,
// not at the last completed Binance daily close the strategy decided on.
//
// Prices are converted to the chain's source units with the symbol map's fixed scale
// (source price = venue mark x venue units per source unit, e.g. x1000 for 1000PEPEUSDT),
// the same scale the gateway uses for quantities. The hourly funding rate is Kraken's
// absolute rate divided by the mark, so it does not depend on the scale.

const krakenMarksCacheTTL = 15 * time.Second

type krakenMark struct {
	Price         float64 // USD per source unit (Binance symbol)
	FundingHourly float64 // fraction of the position value per hour; + = longs pay
	FundingKnown  bool
}

type krakenMarksCache struct {
	mu       sync.Mutex
	value    map[string]krakenMark
	err      error
	finished time.Time
}

// krakenMarks returns the Kraken mark and funding of every mapped source symbol, read at
// most once per krakenMarksCacheTTL.
func (p *Real) krakenMarks(ctx context.Context) (map[string]krakenMark, error) {
	p.krakenMarkCache.mu.Lock()
	defer p.krakenMarkCache.mu.Unlock()
	if !p.krakenMarkCache.finished.IsZero() && time.Since(p.krakenMarkCache.finished) < krakenMarksCacheTTL {
		return p.krakenMarkCache.value, p.krakenMarkCache.err
	}
	p.krakenMarkCache.value, p.krakenMarkCache.err = p.readKrakenMarks(ctx)
	p.krakenMarkCache.finished = time.Now()
	return p.krakenMarkCache.value, p.krakenMarkCache.err
}

func (p *Real) readKrakenMarks(ctx context.Context) (map[string]krakenMark, error) {
	if strings.TrimSpace(p.cfg.SymbolMapFile) == "" {
		return nil, fmt.Errorf("symbol map file is not configured")
	}
	table, err := symbolmap.Load(p.cfg.SymbolMapFile, symbolMapVenue)
	if err != nil {
		return nil, err
	}
	ctx, cancel := context.WithTimeout(ctx, 5*time.Second)
	defer cancel()
	tickers, err := p.kraken.Tickers(ctx)
	if err != nil {
		return nil, err
	}
	return krakenMarksFromTickers(table, tickers), nil
}

func krakenMarksFromTickers(table symbolmap.Table, tickers map[string]krakenpublic.Ticker) map[string]krakenMark {
	marks := make(map[string]krakenMark, len(table.Symbols))
	for _, entry := range table.Symbols {
		ticker, listed := tickers[entry.VenueSymbol]
		if !listed || ticker.Suspended || ticker.MarkPrice <= 0 || entry.VenueUnitsPerSourceUnit <= 0 {
			continue
		}
		mark := krakenMark{Price: ticker.MarkPrice * entry.VenueUnitsPerSourceUnit}
		if ticker.FundingKnown {
			mark.FundingHourly, mark.FundingKnown = ticker.FundingRate/ticker.MarkPrice, true
		}
		marks[entry.Source] = mark
	}
	return marks
}
