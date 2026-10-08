package provider

import (
	"context"
	"fmt"
	"math"
	"sort"
	"strings"
	"time"

	pgstore "control-dashboard-api/internal/integration/postgres"
	sqlitemarket "control-dashboard-api/internal/integration/sqlite"
)

const (
	pureRSIStrategyID      = uint64(1)
	pureRSIMaxSignals      = 10
	pureRSILiquidityLength = 25
	pureRSILength          = 7
	pureRSIEntry           = 80.0
	pureRSIExit            = 70.0
)

type realMarketData struct {
	Source                string                     `json:"source"`
	LatestCompletedCandle string                     `json:"latestCompletedCandle"`
	HealthyAssets         int                        `json:"healthyAssets"`
	StaleAssets           int                        `json:"staleAssets"`
	TotalAssets           int                        `json:"totalAssets"`
	UniverseSize          int                        `json:"universeSize"`
	ActiveSignals         int                        `json:"activeSignals"`
	AvailableSlots        int                        `json:"availableSlots"`
	Freshness             []realMarketFreshness      `json:"freshness"`
	Integrity             realMarketIntegrity        `json:"integrity"`
	Universe              []realUniverseDiagnostic   `json:"universe"`
	CandidateRejections   []realCandidateRejection   `json:"candidateRejections"`
	StrategySnapshot      realMarketStrategySnapshot `json:"strategySnapshot"`
	SourceMode            string                     `json:"sourceMode"`
	SourceUpdatedAt       string                     `json:"sourceUpdatedAt"`
	SourceNote            string                     `json:"sourceNote"`
	SignalDataAvailable   bool                       `json:"signalDataAvailable"`
	SignalCycleAligned    bool                       `json:"signalCycleAligned"`
	CanonicalTopN         int                        `json:"canonicalTopN"`
	LiquidityLabel        string                     `json:"liquidityLabel"`
	HistoryDays           int                        `json:"historyDays"`
}

type realMarketFreshness struct {
	Asset               string `json:"asset"`
	LastCandle          string `json:"lastCandle"`
	AgeLabel            string `json:"ageLabel"`
	StaleThresholdLabel string `json:"staleThresholdLabel"`
	Source              string `json:"source"`
	State               string `json:"state"`
}

type realMarketIntegrity struct {
	MissingCandles      int `json:"missingCandles"`
	DuplicateTimestamps int `json:"duplicateTimestamps"`
	Gaps                int `json:"gaps"`
	InvalidRows         int `json:"invalidRows"`
}

type realUniverseDiagnostic struct {
	Rank           int     `json:"rank"`
	Asset          string  `json:"asset"`
	SMAVolumeLabel string  `json:"smaVolumeLabel"`
	RSI            float64 `json:"rsi"`
	Signal         string  `json:"signal"`
	SignalState    string  `json:"signalState"`
	SlotState      string  `json:"slotState"`
	Diagnostic     string  `json:"diagnostic"`
	DiagnosticTone string  `json:"diagnosticTone"`
}

type realCandidateRejection struct {
	Asset    string `json:"asset"`
	Reason   string `json:"reason"`
	Detail   string `json:"detail"`
	Severity string `json:"severity"`
}

type realMarketStrategySnapshot struct {
	Ranking      string `json:"ranking"`
	EntryRule    string `json:"entryRule"`
	ExitRule     string `json:"exitRule"`
	MaxPositions string `json:"maxPositions"`
	Semantics    string `json:"semantics"`
}

type marketCandidate struct {
	asset     string
	smaVolume float64
	rsi       float64
	bars      []sqlitemarket.Bar
}

func (p *Real) marketDataResource(ctx context.Context) (realMarketData, error) {
	// Coalesce duplicate concurrent reads produced by shell/alerts/registry/rules
	// during a single readiness snapshot. Successful snapshots are reused only
	// briefly; failed reads are shared with current waiters but are not cached for
	// future calls. This avoids self-induced SQLite contention while preserving
	// fail-closed semantics.
	p.marketCache.mu.Lock()
	if !p.marketCache.finished.IsZero() && p.marketCache.err == nil && time.Since(p.marketCache.finished) < dashboardReadCoalesceTTL {
		value := p.marketCache.value
		p.marketCache.mu.Unlock()
		return value, nil
	}
	if p.marketCache.inFlight {
		done := p.marketCache.done
		p.marketCache.mu.Unlock()
		select {
		case <-done:
			p.marketCache.mu.Lock()
			value, err := p.marketCache.value, p.marketCache.err
			p.marketCache.mu.Unlock()
			return value, err
		case <-ctx.Done():
			return realMarketData{}, ctx.Err()
		}
	}
	p.marketCache.inFlight = true
	p.marketCache.done = make(chan struct{})
	done := p.marketCache.done
	p.marketCache.mu.Unlock()

	value, err := p.loadMarketDataResource(ctx)

	p.marketCache.mu.Lock()
	p.marketCache.value = value
	p.marketCache.err = err
	p.marketCache.finished = time.Now()
	p.marketCache.inFlight = false
	close(done)
	p.marketCache.mu.Unlock()
	return value, err
}

func (p *Real) loadMarketDataResource(ctx context.Context) (realMarketData, error) {
	canonicalTopN := p.cfg.MarketTopN
	if canonicalTopN <= 0 {
		canonicalTopN = 50
	}
	historyDays := p.cfg.MarketHistoryDays
	if historyDays <= 0 {
		historyDays = 100
	}
	universeN := p.cfg.StrategyUniverseN
	if universeN <= 0 {
		universeN = 20
	}

	window, err := p.marketData.LoadStrategyWindow(ctx, canonicalTopN, historyDays)
	if err != nil {
		return realMarketData{}, fmt.Errorf("read canonical market-data SQLite window: %w", err)
	}

	strategyCheckpoint, checkpointErr := p.postgres.LatestStrategyCheckpoint(ctx)
	return buildRealMarketData(window, strategyCheckpoint, checkpointErr, canonicalTopN, historyDays, universeN, p.cfg), nil
}

func buildRealMarketData(
	window sqlitemarket.StrategyWindow,
	checkpoint pgstore.StrategyCheckpoint,
	checkpointErr error,
	canonicalTopN, historyDays, universeN int,
	configs ...RealConfig,
) realMarketData {
	cfg := RealConfig{}
	if len(configs) > 0 {
		cfg = configs[0]
	}
	liquidityLabel := "SMA Volume 25"
	if cfg.QuoteVolume {
		liquidityLabel = "SMA Quote Volume 25 (USDT)"
	}
	barsByAsset := make(map[string][]sqlitemarket.Bar)
	for _, bar := range window.Bars {
		barsByAsset[bar.Pair] = append(barsByAsset[bar.Pair], bar)
	}
	for asset := range barsByAsset {
		sort.Slice(barsByAsset[asset], func(i, j int) bool { return barsByAsset[asset][i].Date < barsByAsset[asset][j].Date })
	}

	candidates := make([]marketCandidate, 0, len(window.Ranking))
	missingQuoteRows := 0
	for _, ranked := range window.Ranking {
		bars := barsByAsset[ranked.Pair]
		if !hasBarOnDate(bars, window.LatestDate) {
			continue
		}
		if cfg.QuoteVolume && len(bars) >= pureRSILiquidityLength {
			for _, bar := range bars[len(bars)-pureRSILiquidityLength:] {
				if !isFinite(liquidityValue(bar, true)) {
					missingQuoteRows++
				}
			}
		}
		sma := latestSMAOnRows(bars, pureRSILiquidityLength, func(bar sqlitemarket.Bar) float64 { return liquidityValue(bar, cfg.QuoteVolume) })
		if !isFinite(sma) {
			continue
		}
		candidates = append(candidates, marketCandidate{
			asset: ranked.Pair, smaVolume: sma, rsi: latestRSIOnRows(bars, pureRSILength), bars: bars,
		})
	}
	sort.Slice(candidates, func(i, j int) bool {
		if candidates[i].smaVolume == candidates[j].smaVolume {
			return candidates[i].asset < candidates[j].asset
		}
		return candidates[i].smaVolume > candidates[j].smaVolume
	})
	if universeN > len(candidates) {
		universeN = len(candidates)
	}
	universe := append([]marketCandidate(nil), candidates[:universeN]...)

	aligned := checkpointErr == nil && checkpoint.Timestamp == window.LatestDate && checkpoint.Update.CompletedThrough == window.LatestDate && checkpoint.Intents.Timestamp == window.LatestDate && int(checkpoint.Update.ActiveTopN) == canonicalTopN
	if cfg.QuoteVolume && cfg.QuoteVolumeFrom != "" && fmt.Sprintf("%08d", checkpoint.Timestamp) < cfg.QuoteVolumeFrom {
		aligned = false
	}
	signals := map[string]float64{}
	activeSignals := 0
	if aligned {
		for _, strategy := range checkpoint.Intents.Strategies {
			if strategy.StrategyID != pureRSIStrategyID {
				continue
			}
			for asset, signal := range strategy.Signals {
				signals[asset] = signal
				if math.Abs(signal) > 1e-12 {
					activeSignals++
				}
			}
			break
		}
	}
	availableSlots := 0
	if aligned {
		availableSlots = pureRSIMaxSignals - activeSignals
		if availableSlots < 0 {
			availableSlots = 0
		}
	}

	rows := make([]realUniverseDiagnostic, 0, len(universe))
	rejections := make([]realCandidateRejection, 0)
	freshness := make([]realMarketFreshness, 0, len(universe))
	healthy := 0
	stale := 0
	missingCandles := 0
	gapCount := 0

	for i, candidate := range universe {
		latestBar := latestBar(candidate.bars)
		state := "STALE"
		ageLabel := "no completed candle"
		lastCandle := "—"
		if latestBar != nil {
			lastCandle = formatMarketDate(latestBar.Date)
			days := calendarDaysBetween(latestBar.Date, window.LatestDate)
			if days == 0 {
				state = "HEALTHY"
				ageLabel = "current canonical day"
				healthy++
			} else {
				ageLabel = fmt.Sprintf("%d calendar day(s) behind", days)
				stale++
			}
		} else {
			stale++
		}
		freshness = append(freshness, realMarketFreshness{
			Asset: candidate.asset, LastCandle: lastCandle, AgeLabel: ageLabel,
			StaleThresholdLabel: "target-day candle required", Source: "canonical SQLite", State: state,
		})

		missing, gaps := recentCalendarIntegrity(candidate.bars, window.LatestDate, pureRSILiquidityLength)
		missingCandles += missing
		gapCount += gaps

		signalValue := 0.0
		if aligned {
			signalValue = signals[candidate.asset]
		}
		signal, signalState, slotState, diagnostic, tone := describeMarketSignal(candidate.rsi, signalValue, aligned, activeSignals)
		rows = append(rows, realUniverseDiagnostic{
			Rank: i + 1, Asset: candidate.asset, SMAVolumeLabel: formatCompactVolume(candidate.smaVolume),
			RSI: safeDisplayRSI(candidate.rsi), Signal: signal, SignalState: signalState,
			SlotState: slotState, Diagnostic: diagnostic, DiagnosticTone: tone,
		})

		if aligned && signalValue <= 1e-12 && isFinite(candidate.rsi) && candidate.rsi > pureRSIEntry {
			if activeSignals >= pureRSIMaxSignals {
				rejections = append(rejections, realCandidateRejection{
					Asset: candidate.asset, Reason: "MAX ACTIVE SIGNALS",
					Detail: "RSI entry condition is met, but the durable final signal is flat while the 10-signal cap is occupied.", Severity: "WARN",
				})
			} else {
				rejections = append(rejections, realCandidateRejection{
					Asset: candidate.asset, Reason: "SIGNAL STATE MISMATCH",
					Detail: "Recomputed RSI entry condition is met but the aligned durable StrategyIntent is flat. Investigate before treating the indicator diagnostic as authoritative.", Severity: "WARN",
				})
			}
		}
	}

	source := "canonical SQLite"
	if aligned && strings.TrimSpace(checkpoint.Update.Source) != "" {
		source = checkpoint.Update.Source + " → canonical SQLite"
	}
	note := fmt.Sprintf("OHLCV and the daily exchange ranking are read directly from the canonical SQLite database. %s and RSI(7) are recomputed with the same formulas over the same bounded %d-calendar-day strategy window. Durable StrategyIntent state is joined only when its checkpoint timestamp exactly matches the SQLite ranking frontier.", liquidityLabel, historyDays)
	if cfg.QuoteVolumeFrom != "" {
		note += " Quote-volume strategy takes effect from completed candle " + cfg.QuoteVolumeFrom + "; earlier durable cycles are preserved."
	}
	if !aligned {
		note += " The latest Strategy checkpoint is missing or not cycle-aligned, so this page does not mix its signals into the current market cycle."
	}

	return realMarketData{
		Source:                source,
		LatestCompletedCandle: formatMarketDate(window.LatestDate),
		HealthyAssets:         healthy,
		StaleAssets:           stale,
		TotalAssets:           len(universe),
		UniverseSize:          len(universe),
		ActiveSignals:         activeSignals,
		AvailableSlots:        availableSlots,
		Freshness:             freshness,
		Integrity: realMarketIntegrity{
			MissingCandles:      missingCandles,
			DuplicateTimestamps: window.Integrity.DuplicateRows,
			Gaps:                gapCount,
			InvalidRows:         window.Integrity.InvalidRows + missingQuoteRows,
		},
		Universe:            rows,
		CandidateRejections: rejections,
		StrategySnapshot: realMarketStrategySnapshot{
			Ranking:      fmt.Sprintf("Top %d by %s inside canonical top-%d", universeN, liquidityLabel, canonicalTopN),
			EntryRule:    "RSI(7) > 80",
			ExitRule:     "RSI(7) < 70",
			MaxPositions: "10 persistent signals",
			Semantics:    "LEVEL",
		},
		SourceMode:          "REAL",
		SourceUpdatedAt:     formatMarketDate(window.LatestDate),
		SourceNote:          note,
		SignalDataAvailable: checkpointErr == nil,
		SignalCycleAligned:  aligned,
		CanonicalTopN:       canonicalTopN,
		HistoryDays:         historyDays,
		LiquidityLabel:      liquidityLabel,
	}
}

func describeMarketSignal(rsi, signal float64, aligned bool, activeSignals int) (string, string, string, string, string) {
	if !aligned {
		return "FLAT", "NOT ALIGNED", "UNKNOWN", "Indicator is real; durable signal state is not joined because its checkpoint is not aligned to this market cycle.", "warn"
	}
	if signal > 1e-12 {
		if isFinite(rsi) && rsi < pureRSIExit {
			return "LONG", "ACTIVE", "OCCUPIED", "Persisted long signal is active, but recomputed RSI is below the exit threshold; inspect cycle timing if this persists.", "warn"
		}
		return "LONG", "ACTIVE", "OCCUPIED", "Persisted PureRSI long signal is active for this exact cycle.", "normal"
	}
	if signal < -1e-12 {
		return "SHORT", "ACTIVE", "OCCUPIED", "Unexpected negative PureRSI signal observed in the durable checkpoint.", "bad"
	}
	if !isFinite(rsi) {
		return "FLAT", "NO INDICATOR", "FREE", "Insufficient bounded history to compute RSI(7).", "warn"
	}
	if rsi > pureRSIEntry {
		if activeSignals >= pureRSIMaxSignals {
			return "FLAT", "BLOCKED", "FREE", "Entry condition is met, but the persistent signal cap is already occupied.", "warn"
		}
		return "FLAT", "CHECK", "FREE", "Entry condition is met but the aligned durable signal is flat; investigate checkpoint/config parity.", "bad"
	}
	if rsi < pureRSIExit {
		return "FLAT", "FLAT", "FREE", "Below the exit threshold; no long exposure requested.", "normal"
	}
	return "FLAT", "WAIT", "FREE", "Between entry and exit thresholds with no active persistent signal.", "normal"
}

func liquidityValue(bar sqlitemarket.Bar, quote bool) float64 {
	if !quote {
		return bar.Volume
	}
	if bar.QuoteVolume == nil || !isFinite(*bar.QuoteVolume) || *bar.QuoteVolume < 0 {
		return math.NaN()
	}
	return *bar.QuoteVolume
}

func latestSMAOnRows(bars []sqlitemarket.Bar, length int, value func(sqlitemarket.Bar) float64) float64 {
	if length <= 0 || len(bars) < length {
		return math.NaN()
	}
	sum := 0.0
	for _, bar := range bars[len(bars)-length:] {
		sum += value(bar)
	}
	return sum / float64(length)
}

// latestRSIOnRows mirrors calculateRSI in indicator_calculators.cpp: initial
// simple average followed by Wilder smoothing.
func latestRSIOnRows(bars []sqlitemarket.Bar, length int) float64 {
	if length <= 0 || len(bars) <= length {
		return math.NaN()
	}
	gainSum, lossSum := 0.0, 0.0
	for i := 1; i <= length; i++ {
		change := bars[i].Close - bars[i-1].Close
		if change > 0 {
			gainSum += change
		} else {
			lossSum += math.Abs(change)
		}
	}
	avgGain := gainSum / float64(length)
	avgLoss := lossSum / float64(length)
	rsi := rsiValue(avgGain, avgLoss)
	for i := length + 1; i < len(bars); i++ {
		change := bars[i].Close - bars[i-1].Close
		gain, loss := 0.0, 0.0
		if change > 0 {
			gain = change
		} else if change < 0 {
			loss = math.Abs(change)
		}
		avgGain = ((avgGain * float64(length-1)) + gain) / float64(length)
		avgLoss = ((avgLoss * float64(length-1)) + loss) / float64(length)
		rsi = rsiValue(avgGain, avgLoss)
	}
	return rsi
}

func rsiValue(avgGain, avgLoss float64) float64 {
	if avgGain == 0 && avgLoss == 0 {
		return 50
	}
	if avgLoss == 0 {
		return 100
	}
	if avgGain == 0 {
		return 0
	}
	rs := avgGain / avgLoss
	return 100 - (100 / (1 + rs))
}

func latestBar(bars []sqlitemarket.Bar) *sqlitemarket.Bar {
	if len(bars) == 0 {
		return nil
	}
	return &bars[len(bars)-1]
}

func hasBarOnDate(bars []sqlitemarket.Bar, date uint64) bool {
	for i := len(bars) - 1; i >= 0; i-- {
		if bars[i].Date == date {
			return true
		}
		if bars[i].Date < date {
			break
		}
	}
	return false
}

func recentCalendarIntegrity(bars []sqlitemarket.Bar, latestDate uint64, days int) (int, int) {
	if days <= 0 {
		return 0, 0
	}
	latest, ok := parseMarketDate(latestDate)
	if !ok {
		return 0, 0
	}
	start := latest.AddDate(0, 0, -(days - 1))
	present := make(map[string]struct{}, len(bars))
	for _, bar := range bars {
		date, ok := parseMarketDate(bar.Date)
		if !ok || date.Before(start) || date.After(latest) {
			continue
		}
		present[date.Format("20060102")] = struct{}{}
	}
	missing := 0
	gapRuns := 0
	inGap := false
	for date := start; !date.After(latest); date = date.AddDate(0, 0, 1) {
		_, exists := present[date.Format("20060102")]
		if !exists {
			missing++
			if !inGap {
				gapRuns++
				inGap = true
			}
		} else {
			inGap = false
		}
	}
	return missing, gapRuns
}

func formatMarketDate(value uint64) string {
	date, ok := parseMarketDate(value)
	if !ok {
		return fmt.Sprintf("%d", value)
	}
	return date.Format("2006-01-02")
}

func parseMarketDate(value uint64) (time.Time, bool) {
	date, err := time.Parse("20060102", fmt.Sprintf("%08d", value))
	return date, err == nil
}

func calendarDaysBetween(from, to uint64) int {
	left, okLeft := parseMarketDate(from)
	right, okRight := parseMarketDate(to)
	if !okLeft || !okRight || left.After(right) {
		return 0
	}
	return int(right.Sub(left).Hours() / 24)
}

func safeDisplayRSI(value float64) float64 {
	if !isFinite(value) {
		return 0
	}
	return value
}

func isFinite(value float64) bool {
	return !math.IsNaN(value) && !math.IsInf(value, 0)
}

func formatCompactVolume(value float64) string {
	abs := math.Abs(value)
	suffix := ""
	scale := 1.0
	switch {
	case abs >= 1_000_000_000:
		suffix, scale = "B", 1_000_000_000
	case abs >= 1_000_000:
		suffix, scale = "M", 1_000_000
	case abs >= 1_000:
		suffix, scale = "K", 1_000
	}
	if suffix == "" {
		return fmt.Sprintf("%.2f", value)
	}
	return fmt.Sprintf("%.2f%s", value/scale, suffix)
}
