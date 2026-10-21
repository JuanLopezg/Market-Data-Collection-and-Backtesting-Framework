package provider

import (
	"context"
	"fmt"
	"math/rand/v2"
	"sort"
	"strings"
	"sync"
	"time"

	krakenpublic "control-dashboard-api/internal/integration/kraken"
	"control-dashboard-api/internal/integration/symbolmap"
)

// Symbol mapping check: can every asset the strategy may trade be sent to Kraken, and does
// the unit scale in the symbol table agree with real prices?
//
// The table (config/venues/kraken_futures/symbol_map.json) is the one the exchange gateway
// uses to convert orders. For each asset in the strategy universe and the market-data top
// ranking it compares the latest Binance daily close with the current Kraken mark price:
//
//	Binance close / Kraken mark  should be about  venue_units_per_source_unit
//
// (1 for BTCUSDT -> PF_XBTUSD, 1000 for 1000PEPEUSDT -> PF_PEPEUSD). A wrong scale shows up
// as a ratio off by its factor, at least 1000 (the only prefixes are 1000, 1000000 and 1M).
// The close can be up to a day old, so the tolerance is wide (5x): a coin that moved 80% since
// the close is not a wrong table (ARC fell ~45% on 2026-10-13 and tripped the earlier 1.5x).
// The gateway keeps 1.5x because it compares prices taken minutes apart.
//
// For a symbol without a table entry it also looks for a Kraken contract with the same name
// (or without an explicit 1000/1000000/1M prefix) whose price confirms the scale. Such a
// symbol is MISSING_FROM_TABLE: Kraken lists it but the table is out of date. This is only
// a hint to rebuild the table with tools/kraken_symbol_map.py; the gateway still trades
// nothing that is not in the reviewed table.
const (
	symbolMapVenue         = "kraken-futures"
	symbolMapMaxPriceRatio = 5.0
	// Signals change once a day and Kraken listings rarely: a 4.5-5.5 min refresh is
	// enough, and this check reads the whole market window and Kraken's prices.
	symbolMapCacheTTL       = 5 * time.Minute
	symbolMapCacheJitter    = time.Minute
	symbolMapCheckTimeout   = 8 * time.Second
	symbolMapStateOK        = "OK"
	symbolMapStateNotMapped = "NOT_ON_VENUE"
	symbolMapStateMissing   = "MISSING_FROM_TABLE"
	symbolMapStateNotListed = "NOT_LISTED"
	symbolMapStateSuspended = "SUSPENDED"
	symbolMapStateNoPrice   = "NO_PRICE"
	symbolMapStateMismatch  = "SCALE_MISMATCH"
)

// Same name rules as tools/kraken_symbol_map.py: only an explicit scale prefix followed by
// a letter is removed (1INCH stays 1INCH).
var symbolMapScalePrefixes = []struct {
	prefix string
	scale  float64
}{{"1000000", 1_000_000}, {"1000", 1000}, {"1M", 1_000_000}}

type SymbolMapRow struct {
	Source       string  `json:"source"`
	Asset        string  `json:"asset,omitempty"`
	VenueSymbol  string  `json:"venueSymbol,omitempty"`
	Scale        float64 `json:"scale,omitempty"`
	SourcePrice  float64 `json:"sourcePrice,omitempty"`
	VenuePrice   float64 `json:"venuePrice,omitempty"`
	PriceRatio   float64 `json:"priceRatio,omitempty"`
	InStrategy   bool    `json:"inStrategy"`
	ActiveSignal bool    `json:"activeSignal"`
	InRanking    bool    `json:"inRanking"`
	State        string  `json:"state"`
	Detail       string  `json:"detail"`
	tradeable    bool
}

type SymbolMapAlarm struct {
	Severity  string `json:"severity"`
	EventType string `json:"eventType"`
	Asset     string `json:"asset,omitempty"`
	Title     string `json:"title"`
	Detail    string `json:"detail"`
}

type SymbolMapStatus struct {
	// OK, WARN (some strategy assets cannot be traded, even with an active signal: only
	// those orders are skipped), BLOCKED (wrong scale) or UNAVAILABLE (table or prices
	// could not be read).
	Status           string           `json:"status"`
	Venue            string           `json:"venue"`
	TableGeneratedAt string           `json:"tableGeneratedAt,omitempty"`
	TableEntries     int              `json:"tableEntries"`
	StrategyAssets   int              `json:"strategyAssets"`
	StrategyMapped   int              `json:"strategyMapped"`
	CheckedAt        string           `json:"checkedAt"`
	Error            string           `json:"error,omitempty"`
	Rows             []SymbolMapRow   `json:"rows"`
	Alarms           []SymbolMapAlarm `json:"alarms"`
}

type symbolMapCache struct {
	mu        sync.Mutex
	value     SymbolMapStatus
	expiresAt time.Time
}

// symbolMapInput is what the check needs, gathered from the table, market data and Kraken.
type symbolMapInput struct {
	table         symbolmap.Table
	strategy      []string        // Current strategy universe, Binance symbols.
	activeSignals map[string]bool // Strategy assets with an active long/short signal.
	ranking       []string        // Market-data top ranking, Binance symbols.
	closes        map[string]float64
	tickers       map[string]krakenpublic.Ticker
}

func buildSymbolMapStatus(in symbolMapInput, checkedAt time.Time) SymbolMapStatus {
	result := SymbolMapStatus{Status: "OK", Venue: symbolMapVenue, TableGeneratedAt: in.table.GeneratedAt,
		TableEntries: len(in.table.Symbols), CheckedAt: checkedAt.UTC().Format(time.RFC3339),
		Rows: []SymbolMapRow{}, Alarms: []SymbolMapAlarm{}}

	rows := map[string]*SymbolMapRow{}
	var order []string
	add := func(symbol string) *SymbolMapRow {
		symbol = strings.TrimSpace(symbol)
		if row, ok := rows[symbol]; ok {
			return row
		}
		rows[symbol] = &SymbolMapRow{Source: symbol}
		order = append(order, symbol)
		return rows[symbol]
	}
	for _, symbol := range in.strategy {
		row := add(symbol)
		row.InStrategy = true
		row.ActiveSignal = in.activeSignals[symbol]
	}
	for _, symbol := range in.ranking {
		add(symbol).InRanking = true
	}

	for _, symbol := range order {
		row := rows[symbol]
		checkSymbolMapRow(row, in)
		if row.InStrategy {
			result.StrategyAssets++
			if row.tradeable {
				result.StrategyMapped++
			}
		}
		if alarm, ok := symbolMapAlarm(*row); ok {
			result.Alarms = append(result.Alarms, alarm)
		}
		result.Rows = append(result.Rows, *row)
	}

	// Strategy assets first, then the rest of the ranking; alphabetical inside each group.
	sort.SliceStable(result.Rows, func(i, j int) bool {
		if result.Rows[i].InStrategy != result.Rows[j].InStrategy {
			return result.Rows[i].InStrategy
		}
		return result.Rows[i].Source < result.Rows[j].Source
	})
	// INFO alarms (top-50 assets Kraken does not list) do not change the status.
	for _, alarm := range result.Alarms {
		switch alarm.Severity {
		case "CRITICAL":
			result.Status = "BLOCKED"
		case "ALERT", "WARN":
			if result.Status == "OK" {
				result.Status = "WARN"
			}
		}
	}
	return result
}

func checkSymbolMapRow(row *SymbolMapRow, in symbolMapInput) {
	row.SourcePrice = in.closes[row.Source]
	entry, mapped := in.table.BySource(row.Source)
	if !mapped {
		checkUnmappedSymbol(row, in)
		return
	}
	row.Asset, row.VenueSymbol, row.Scale = entry.Asset, entry.VenueSymbol, entry.VenueUnitsPerSourceUnit

	ticker, listed := in.tickers[entry.VenueSymbol]
	switch {
	case !listed:
		row.State = symbolMapStateNotListed
		row.Detail = entry.VenueSymbol + " is in the symbol table but Kraken no longer lists it."
		return
	case ticker.Suspended:
		row.State = symbolMapStateSuspended
		row.Detail = entry.VenueSymbol + " is suspended on Kraken."
		return
	}
	row.VenuePrice = ticker.MarkPrice
	if row.SourcePrice <= 0 || row.VenuePrice <= 0 {
		row.State = symbolMapStateNoPrice
		row.Detail = "A price is missing, so the unit scale cannot be checked."
		return
	}
	row.PriceRatio = row.SourcePrice / row.VenuePrice
	deviation := row.PriceRatio / row.Scale
	if deviation > symbolMapMaxPriceRatio || deviation < 1/symbolMapMaxPriceRatio {
		row.State = symbolMapStateMismatch
		row.Detail = fmt.Sprintf("Binance/Kraken price ratio is %.4g but the table says x%g; the table or a contract is wrong.",
			row.PriceRatio, row.Scale)
		return
	}
	row.State = symbolMapStateOK
	row.tradeable = true
	if row.Scale == 1 {
		row.Detail = fmt.Sprintf("1 %s = 1 %s on %s.", row.Source, entry.Asset, entry.VenueSymbol)
	} else {
		row.Detail = fmt.Sprintf("1 %s = %g %s on %s.", row.Source, row.Scale, entry.Asset, entry.VenueSymbol)
	}
}

// checkUnmappedSymbol tells "Kraken does not list it" from "the table is out of date": a
// same-named Kraken contract (Binance base, or base without its scale prefix) whose price
// confirms the scale makes the row MISSING_FROM_TABLE, with the proposed contract shown.
func checkUnmappedSymbol(row *SymbolMapRow, in symbolMapInput) {
	row.State = symbolMapStateNotMapped
	row.Detail = "Kraken lists no perpetual for this asset, so the symbol table has no entry."

	base := strings.TrimSuffix(row.Source, "USDT")
	if base == row.Source || base == "" || row.SourcePrice <= 0 {
		return
	}
	type candidate struct {
		asset string
		scale float64
	}
	candidates := []candidate{{base, 1}}
	for _, prefix := range symbolMapScalePrefixes {
		rest := strings.TrimPrefix(base, prefix.prefix)
		if rest != base && rest != "" && rest[0] >= 'A' && rest[0] <= 'Z' {
			candidates = append(candidates, candidate{rest, prefix.scale})
			break
		}
	}
	for _, c := range candidates {
		name := c.asset
		if name == "BTC" {
			name = "XBT" // Kraken's code for BTC.
		}
		ticker, listed := in.tickers["PF_"+name+"USD"]
		if !listed || ticker.Suspended || ticker.MarkPrice <= 0 {
			continue
		}
		deviation := row.SourcePrice / ticker.MarkPrice / c.scale
		if deviation > symbolMapMaxPriceRatio || deviation < 1/symbolMapMaxPriceRatio {
			continue // Same name, different token or scale: not proposed.
		}
		row.State = symbolMapStateMissing
		row.Asset, row.VenueSymbol, row.Scale = c.asset, ticker.Symbol, c.scale
		row.VenuePrice = ticker.MarkPrice
		row.PriceRatio = row.SourcePrice / ticker.MarkPrice
		row.Detail = fmt.Sprintf("Kraken lists %s (1 %s = %g %s by name and price) but the symbol table has no entry; "+
			"rebuild it with tools/kraken_symbol_map.py.", ticker.Symbol, row.Source, c.scale, c.asset)
		return
	}
}

// Alarms:
//   - a wrong scale is CRITICAL: the table itself is wrong;
//   - a universe asset Kraken cannot take is WARN, ALERT while its signal is active,
//     because that order would be rejected;
//   - a top-ranking asset missing from an out-of-date table is WARN (rebuild the table);
//   - a top-ranking asset Kraken does not list is INFO: visible, but not a warning,
//     since a dozen top-50 assets are permanently absent from Kraken.
func symbolMapAlarm(row SymbolMapRow) (SymbolMapAlarm, bool) {
	if row.State == symbolMapStateMismatch {
		return SymbolMapAlarm{Severity: "CRITICAL", EventType: "SYMBOL_MAP_SCALE_MISMATCH", Asset: row.Source,
			Title: "Symbol table scale disagrees with prices", Detail: row.Detail}, true
	}
	if row.tradeable || row.State == symbolMapStateNoPrice {
		return SymbolMapAlarm{}, false
	}
	if !row.InStrategy {
		if row.State == symbolMapStateMissing {
			return SymbolMapAlarm{Severity: "WARN", EventType: "SYMBOL_MAP_TABLE_OUT_OF_DATE", Asset: row.Source,
				Title: "Top-50 asset missing from the symbol table", Detail: row.Detail}, true
		}
		return SymbolMapAlarm{Severity: "INFO", EventType: "SYMBOL_MAP_RANKING_NOT_TRADEABLE", Asset: row.Source,
			Title:  "Top-50 asset not tradeable on Kraken",
			Detail: row.Detail + " If the strategy selects it, its signals will not be executed."}, true
	}
	if row.ActiveSignal {
		return SymbolMapAlarm{Severity: "ALERT", EventType: "SYMBOL_MAP_SIGNAL_NOT_TRADEABLE", Asset: row.Source,
			Title:  "Active signal on an asset Kraken cannot trade",
			Detail: row.Detail + " Its orders would be rejected by the exchange gateway."}, true
	}
	return SymbolMapAlarm{Severity: "WARN", EventType: "SYMBOL_MAP_STRATEGY_NOT_TRADEABLE", Asset: row.Source,
		Title:  "Strategy asset not tradeable on Kraken",
		Detail: row.Detail + " A signal on it would not be executed."}, true
}

func unavailableSymbolMapStatus(err error, checkedAt time.Time) SymbolMapStatus {
	return SymbolMapStatus{Status: "UNAVAILABLE", Venue: symbolMapVenue, CheckedAt: checkedAt.UTC().Format(time.RFC3339),
		Error: err.Error(), Rows: []SymbolMapRow{}, Alarms: []SymbolMapAlarm{}}
}

// SymbolMapStatus refreshes about every 5 minutes; Kraken is asked once per run.
// Completion-relative jitter prevents multiple processes from retaining the
// same refresh phase after starting together.
func (p *Real) SymbolMapStatus(ctx context.Context) SymbolMapStatus {
	p.symbolCache.mu.Lock()
	defer p.symbolCache.mu.Unlock()
	if time.Now().Before(p.symbolCache.expiresAt) {
		return p.symbolCache.value
	}
	ctx, cancel := context.WithTimeout(ctx, symbolMapCheckTimeout)
	defer cancel()
	p.symbolCache.value = p.checkSymbolMap(ctx)
	p.symbolCache.expiresAt = time.Now().Add(symbolMapCacheTTL - symbolMapCacheJitter/2 + time.Duration(rand.Int64N(int64(symbolMapCacheJitter))))
	return p.symbolCache.value
}

func (p *Real) checkSymbolMap(ctx context.Context) SymbolMapStatus {
	now := time.Now().UTC()
	if strings.TrimSpace(p.cfg.SymbolMapFile) == "" {
		return unavailableSymbolMapStatus(fmt.Errorf("symbol map file is not configured"), now)
	}
	table, err := symbolmap.Load(p.cfg.SymbolMapFile, symbolMapVenue)
	if err != nil {
		return unavailableSymbolMapStatus(err, now)
	}
	market, err := p.marketDataResource(ctx)
	if err != nil {
		return unavailableSymbolMapStatus(fmt.Errorf("strategy universe unavailable: %w", err), now)
	}
	window, err := p.marketData.LoadStrategyWindow(ctx, maxInt(p.cfg.MarketTopN, 50), maxInt(p.cfg.MarketHistoryDays, 100))
	if err != nil {
		return unavailableSymbolMapStatus(fmt.Errorf("market-data ranking unavailable: %w", err), now)
	}
	tickers, err := p.kraken.Tickers(ctx)
	if err != nil {
		return unavailableSymbolMapStatus(fmt.Errorf("Kraken prices unavailable: %w", err), now)
	}

	in := symbolMapInput{table: table, activeSignals: map[string]bool{}, closes: map[string]float64{}, tickers: tickers}
	for _, row := range market.Universe {
		in.strategy = append(in.strategy, row.Asset)
		in.activeSignals[row.Asset] = row.SignalState == "ACTIVE"
	}
	for _, row := range window.Ranking {
		in.ranking = append(in.ranking, row.Pair)
	}
	latest := map[string]uint64{}
	for _, bar := range window.Bars {
		if bar.Date >= latest[bar.Pair] {
			latest[bar.Pair] = bar.Date
			in.closes[bar.Pair] = bar.Close
		}
	}
	return buildSymbolMapStatus(in, now)
}

func (p *EmbeddedMock) SymbolMapStatus(_ context.Context) SymbolMapStatus {
	return SymbolMapStatus{Status: "UNAVAILABLE", Venue: symbolMapVenue, CheckedAt: "mock",
		Error: "Mock mode has no symbol table or Kraken prices.", Rows: []SymbolMapRow{}, Alarms: []SymbolMapAlarm{}}
}

func maxInt(a, b int) int {
	if a > b {
		return a
	}
	return b
}
