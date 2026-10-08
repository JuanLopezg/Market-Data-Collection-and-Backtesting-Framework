package provider

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"math"
	"sort"
	"strings"
	"time"

	sqlitemarket "control-dashboard-api/internal/integration/sqlite"
)

type realLiveVsExpected struct {
	ContractVersion          string                       `json:"contractVersion"`
	Status                   string                       `json:"status"`
	Validated                bool                         `json:"validated"`
	ProjectionReady          bool                         `json:"projectionReady"`
	ObservationMode          string                       `json:"observationMode"`
	BaselineLabel            string                       `json:"baselineLabel"`
	BaselineWindow           string                       `json:"baselineWindow"`
	BaselineFingerprint      string                       `json:"baselineFingerprint"`
	BaselineObservationCount int                          `json:"baselineObservationCount"`
	BaselineExcludesLatest   bool                         `json:"baselineExcludesLatest"`
	LatestObservation        string                       `json:"latestObservation"`
	MetricCount              int                          `json:"metricCount"`
	NormalCount              int                          `json:"normalCount"`
	ElevatedCount            int                          `json:"elevatedCount"`
	AbnormalCount            int                          `json:"abnormalCount"`
	CriticalCount            int                          `json:"criticalCount"`
	OverallClassification    string                       `json:"overallClassification"`
	AnomalyCount             int                          `json:"anomalyCount"`
	Anomalies                []realLiveVsExpectedAnomaly  `json:"anomalies"`
	Coverage                 []realLiveVsExpectedCoverage `json:"coverage"`
	Metrics                  []realBehaviourMetric        `json:"metrics"`
	SourceMode               string                       `json:"sourceMode"`
	SourceNote               string                       `json:"sourceNote"`
	ReadOnly                 bool                         `json:"readOnly"`
	PrivateAuth              string                       `json:"privateAuth"`
	OrderRouting             string                       `json:"orderRouting"`
	CheckedAt                string                       `json:"checkedAt"`
	Error                    string                       `json:"error,omitempty"`
}

type realLiveVsExpectedCoverage struct {
	ID     string `json:"id"`
	Label  string `json:"label"`
	State  string `json:"state"`
	Source string `json:"source"`
	Detail string `json:"detail"`
}

type realLiveVsExpectedAnomaly struct {
	MetricID       string  `json:"metricId"`
	Label          string  `json:"label"`
	Classification string  `json:"classification"`
	ZScore         float64 `json:"zScore"`
	CurrentLabel   string  `json:"currentLabel"`
	Detail         string  `json:"detail"`
}

type realBehaviourTrendPoint struct {
	Label string  `json:"label"`
	Live  float64 `json:"live"`
	Mean  float64 `json:"mean"`
	Lower float64 `json:"lower"`
	Upper float64 `json:"upper"`
}

type realBehaviourContribution struct {
	Timestamp      string `json:"timestamp"`
	Asset          string `json:"asset"`
	CycleID        string `json:"cycleId"`
	ValueLabel     string `json:"valueLabel"`
	DeviationLabel string `json:"deviationLabel"`
}

type realBehaviourMetric struct {
	ID                   string                      `json:"id"`
	Family               string                      `json:"family"`
	Label                string                      `json:"label"`
	CurrentLabel         string                      `json:"currentLabel"`
	HistoricalMeanLabel  string                      `json:"historicalMeanLabel"`
	HistoricalRangeLabel string                      `json:"historicalRangeLabel"`
	ZScore               float64                     `json:"zScore"`
	Classification       string                      `json:"classification"`
	Description          string                      `json:"description"`
	Trend                []realBehaviourTrendPoint   `json:"trend"`
	Distribution         []int                       `json:"distribution"`
	ContributingCycles   []realBehaviourContribution `json:"contributingCycles"`
}

type dailyBehaviourObservation struct {
	Date                  uint64
	Universe              []string
	EntryQualified        float64
	MedianRSI             float64
	HighRSISharePct       float64
	UniverseTurnoverPct   float64
	LiquidityTop5SharePct float64
}

type behaviourSeries struct {
	ID          string
	Family      string
	Label       string
	Description string
	Values      []datedValue
	Format      func(float64) string
}

type datedValue struct {
	Date  uint64
	Value float64
}

func (p *Real) liveVsExpected(ctx context.Context) (realLiveVsExpected, error) {
	canonicalTopN := p.cfg.MarketTopN
	if canonicalTopN <= 0 {
		canonicalTopN = 50
	}
	historyDays := p.cfg.MarketHistoryDays
	if historyDays < 60 {
		historyDays = 60
	}
	if historyDays > 365 {
		historyDays = 365
	}
	universeN := p.cfg.StrategyUniverseN
	if universeN <= 0 {
		universeN = 20
	}

	window, err := p.marketData.LoadBehaviourWindow(ctx, canonicalTopN, historyDays, 35)
	if err != nil {
		return realLiveVsExpected{}, fmt.Errorf("read canonical behaviour baseline window: %w", err)
	}
	return buildRealLiveVsExpected(window, canonicalTopN, universeN, p.cfg), nil
}

func step45Coverage(marketState, marketDetail string) []realLiveVsExpectedCoverage {
	return []realLiveVsExpectedCoverage{
		{ID: "market-inputs", Label: "Canonical market / strategy-input behaviour", State: marketState, Source: "SQLite market_volume_rank_daily + ohlcv_data", Detail: marketDetail},
		{ID: "execution-distribution", Label: "Execution slippage / latency / rejects", State: "DEFERRED", Source: "future versioned execution baseline", Detail: "Durable execution observations exist, but no accepted historical execution-distribution artifact is canonical yet; Step 45 does not invent one."},
		{ID: "performance-accounting", Label: "PnL / equity / holding-time behaviour", State: "DEFERRED", Source: "future runtime-owned accounting projection", Detail: "Step 42 explicitly does not claim durable realized/unrealized PnL or historical equity, so performance baselines remain unavailable."},
		{ID: "accepted-replay", Label: "Accepted replay behavioural baseline", State: "DEFERRED", Source: "future versioned replay artifact", Detail: "The future HyperliquidAdapter/MockExchangeAdapter replay path must publish an accepted versioned baseline before replay economics can be compared here."},
	}
}

func buildRealLiveVsExpected(window sqlitemarket.BehaviourWindow, canonicalTopN, universeN int, configs ...RealConfig) realLiveVsExpected {
	cfg := RealConfig{}
	if len(configs) > 0 {
		cfg = configs[0]
	}
	if cfg.QuoteVolumeFrom != "" {
		ranking := make([]sqlitemarket.DailyRankingRow, 0)
		for _, row := range window.Ranking {
			if fmt.Sprintf("%08d", row.Date) >= cfg.QuoteVolumeFrom {
				ranking = append(ranking, row)
			}
		}
		window.Ranking = ranking
	}
	observations := computeDailyBehaviour(window, universeN, cfg.QuoteVolume)
	checkedAt := timeNowUTC()
	if len(observations) < 6 {
		return realLiveVsExpected{
			ContractVersion: "step45-v1", Status: "VALIDATED_LIMITED", Validated: true, ProjectionReady: false,
			ObservationMode: "LATEST_COMPLETED_CANONICAL_MARKET_DAY",
			BaselineLabel:   "Canonical SQLite rolling baseline", BaselineWindow: fmt.Sprintf("Insufficient completed observations (%d)", len(observations)),
			BaselineFingerprint: behaviourBaselineFingerprint(observations, canonicalTopN, universeN), BaselineObservationCount: maxIntStep45(0, len(observations)-1), BaselineExcludesLatest: true,
			LatestObservation: formatMarketDate(window.LatestDate), OverallClassification: "INSUFFICIENT_DATA", Anomalies: []realLiveVsExpectedAnomaly{},
			Coverage: step45Coverage("INSUFFICIENT_DATA", "The REAL canonical source is readable, but at least six completed observations are required before the latest observation can be compared against a rolling baseline."),
			Metrics:  []realBehaviourMetric{}, SourceMode: "REAL", ReadOnly: true, PrivateAuth: "DEFERRED", OrderRouting: "DISABLED", CheckedAt: checkedAt,
			SourceNote: "Step 45 validates the REAL read-only projection contract even when history is still insufficient for anomaly classification. Classification remains withheld until the canonical market database contains at least six completed observations. No PnL/equity, slippage, fill-latency, reject-rate, execution-distribution or accepted-replay historical baseline is inferred.",
		}
	}

	series := []behaviourSeries{
		{
			ID: "entry-qualified", Family: "ACTIVITY", Label: "Entry-qualified assets",
			Description: "Count of the reconstructed top-20 universe with RSI(7) > 80 on each completed canonical market day. This is an input-condition baseline, not a count of persisted strategy transitions.",
			Values:      obsValues(observations, func(o dailyBehaviourObservation) float64 { return o.EntryQualified }),
			Format:      func(v float64) string { return fmt.Sprintf("%.0f assets", v) },
		},
		{
			ID: "universe-turnover", Family: "ACTIVITY", Label: "Universe Turnover",
			Description: "Daily percentage of top-20 liquidity-universe membership that changed versus the previous completed canonical day.",
			Values:      obsValuesSkipNaN(observations, func(o dailyBehaviourObservation) float64 { return o.UniverseTurnoverPct }),
			Format:      func(v float64) string { return fmt.Sprintf("%.1f%%", v) },
		},
		{
			ID: "median-rsi", Family: "STRATEGY INPUTS", Label: "Top-20 Median RSI",
			Description: "Median RSI(7) across the reconstructed top-20 universe, recomputed from canonical OHLCV with Wilder smoothing.",
			Values:      obsValues(observations, func(o dailyBehaviourObservation) float64 { return o.MedianRSI }),
			Format:      func(v float64) string { return fmt.Sprintf("%.1f", v) },
		},
		{
			ID: "high-rsi-share", Family: "STRATEGY INPUTS", Label: "RSI ≥ 70 Share",
			Description: "Share of the reconstructed top-20 universe at or above RSI 70, providing a bounded view of broad strategy-input pressure.",
			Values:      obsValues(observations, func(o dailyBehaviourObservation) float64 { return o.HighRSISharePct }),
			Format:      func(v float64) string { return fmt.Sprintf("%.1f%%", v) },
		},
		{
			ID: "liquidity-concentration", Family: "RISK", Label: "Top-5 Liquidity Concentration",
			Description: "Share of reconstructed top-20 configured SMA liquidity(25) represented by the five largest candidates. It is a market concentration diagnostic, not a portfolio exposure limit.",
			Values:      obsValues(observations, func(o dailyBehaviourObservation) float64 { return o.LiquidityTop5SharePct }),
			Format:      func(v float64) string { return fmt.Sprintf("%.1f%%", v) },
		},
	}

	metrics := make([]realBehaviourMetric, 0, len(series))
	counts := map[string]int{"NORMAL": 0, "ELEVATED": 0, "ABNORMAL": 0, "CRITICAL": 0}
	for _, s := range series {
		metric, ok := buildBehaviourMetric(s)
		if !ok {
			continue
		}
		metrics = append(metrics, metric)
		counts[metric.Classification]++
	}

	firstDate := observations[0].Date
	latestDate := observations[len(observations)-1].Date
	anomalies := make([]realLiveVsExpectedAnomaly, 0, len(metrics))
	overall := "NORMAL"
	for _, metric := range metrics {
		if behaviourClassRank(metric.Classification) > behaviourClassRank(overall) {
			overall = metric.Classification
		}
		if metric.Classification != "NORMAL" {
			anomalies = append(anomalies, realLiveVsExpectedAnomaly{
				MetricID: metric.ID, Label: metric.Label, Classification: metric.Classification, ZScore: metric.ZScore, CurrentLabel: metric.CurrentLabel,
				Detail: "Observed latest completed canonical market day differs from the rolling historical distribution. This is an observability anomaly, not an automatic trading instruction.",
			})
		}
	}
	baseline := observations[:len(observations)-1]
	return realLiveVsExpected{
		ContractVersion: "step45-v1", Status: "VALIDATED_LIMITED", Validated: true, ProjectionReady: true,
		ObservationMode:     "LATEST_COMPLETED_CANONICAL_MARKET_DAY",
		BaselineLabel:       "Canonical SQLite rolling baseline",
		BaselineWindow:      fmt.Sprintf("%d-day bounded window · %s → %s · latest day excluded from baseline statistics · top-%d by configured SMA liquidity(25) inside canonical top-%d", len(observations), formatMarketDate(firstDate), formatMarketDate(latestDate), universeN, canonicalTopN),
		BaselineFingerprint: behaviourBaselineFingerprint(baseline, canonicalTopN, universeN), BaselineObservationCount: len(baseline), BaselineExcludesLatest: true,
		LatestObservation: formatMarketDate(latestDate),
		MetricCount:       len(metrics), NormalCount: counts["NORMAL"], ElevatedCount: counts["ELEVATED"], AbnormalCount: counts["ABNORMAL"], CriticalCount: counts["CRITICAL"],
		OverallClassification: overall, AnomalyCount: len(anomalies), Anomalies: anomalies,
		Coverage: step45Coverage("VALIDATED", fmt.Sprintf("%d real market/strategy-input metrics are classified against an empirical rolling distribution using the latest completed canonical day as the observation.", len(metrics))),
		Metrics:  metrics, SourceMode: "REAL", ReadOnly: true, PrivateAuth: "DEFERRED", OrderRouting: "DISABLED", CheckedAt: checkedAt,
		SourceNote: "Step 45 formalizes the REAL live-vs-expected read model as a versioned, fingerprinted, read-only anomaly projection over canonical market history. It deliberately remains LIMITED: accepted-replay, durable PnL/equity, holding-time, slippage, fill-latency and reject-rate historical distributions are not claimed until their canonical baseline artifacts exist. Classifications are observational and do not enable, disable or recommend trades.",
	}
}

func behaviourClassRank(value string) int {
	switch value {
	case "ELEVATED":
		return 1
	case "ABNORMAL":
		return 2
	case "CRITICAL":
		return 3
	default:
		return 0
	}
}

func behaviourBaselineFingerprint(observations []dailyBehaviourObservation, canonicalTopN, universeN int) string {
	h := sha256.New()
	fmt.Fprintf(h, "step45-v1|canonicalTopN=%d|universeN=%d|observations=%d\n", canonicalTopN, universeN, len(observations))
	for _, o := range observations {
		fmt.Fprintf(h, "%d|%.17g|%.17g|%.17g|%.17g|%.17g|%s\n", o.Date, o.EntryQualified, o.MedianRSI, o.HighRSISharePct, o.UniverseTurnoverPct, o.LiquidityTop5SharePct, strings.Join(o.Universe, ","))
	}
	return hex.EncodeToString(h.Sum(nil))
}

func maxIntStep45(a, b int) int {
	if a > b {
		return a
	}
	return b
}

// timeNowUTC is kept as a tiny seam for the Step 45 contract timestamp without
// allowing wall clock to enter any economic calculation.
func timeNowUTC() string { return time.Now().UTC().Format(time.RFC3339) }

func computeDailyBehaviour(window sqlitemarket.BehaviourWindow, universeN int, quoteVolume ...bool) []dailyBehaviourObservation {
	rankingByDate := make(map[uint64][]sqlitemarket.DailyRankingRow)
	dates := make([]uint64, 0)
	seenDates := make(map[uint64]struct{})
	for _, row := range window.Ranking {
		rankingByDate[row.Date] = append(rankingByDate[row.Date], row)
		if _, ok := seenDates[row.Date]; !ok {
			seenDates[row.Date] = struct{}{}
			dates = append(dates, row.Date)
		}
	}
	sort.Slice(dates, func(i, j int) bool { return dates[i] < dates[j] })

	barsByAsset := make(map[string][]sqlitemarket.Bar)
	for _, bar := range window.Bars {
		barsByAsset[bar.Pair] = append(barsByAsset[bar.Pair], bar)
	}
	for asset := range barsByAsset {
		sort.Slice(barsByAsset[asset], func(i, j int) bool { return barsByAsset[asset][i].Date < barsByAsset[asset][j].Date })
	}

	observations := make([]dailyBehaviourObservation, 0, len(dates))
	var previousUniverse []string
	for _, date := range dates {
		candidates := make([]marketCandidate, 0, len(rankingByDate[date]))
		for _, ranked := range rankingByDate[date] {
			bars := barsThroughDate(barsByAsset[ranked.Pair], date)
			if !hasBarOnDate(bars, date) {
				continue
			}
			sma := latestSMAOnRows(bars, pureRSILiquidityLength, func(bar sqlitemarket.Bar) float64 { return liquidityValue(bar, len(quoteVolume) > 0 && quoteVolume[0]) })
			rsi := latestRSIOnRows(bars, pureRSILength)
			if !isFinite(sma) || !isFinite(rsi) {
				continue
			}
			candidates = append(candidates, marketCandidate{asset: ranked.Pair, smaVolume: sma, rsi: rsi, bars: bars})
		}
		sort.Slice(candidates, func(i, j int) bool {
			if candidates[i].smaVolume == candidates[j].smaVolume {
				return candidates[i].asset < candidates[j].asset
			}
			return candidates[i].smaVolume > candidates[j].smaVolume
		})
		limit := universeN
		if limit > len(candidates) {
			limit = len(candidates)
		}
		if limit == 0 {
			continue
		}
		universe := candidates[:limit]
		assets := make([]string, 0, limit)
		rsis := make([]float64, 0, limit)
		entryQualified := 0
		highRSI := 0
		totalLiquidity := 0.0
		top5Liquidity := 0.0
		for i, candidate := range universe {
			assets = append(assets, candidate.asset)
			rsis = append(rsis, candidate.rsi)
			if candidate.rsi > pureRSIEntry {
				entryQualified++
			}
			if candidate.rsi >= pureRSIExit {
				highRSI++
			}
			totalLiquidity += candidate.smaVolume
			if i < 5 {
				top5Liquidity += candidate.smaVolume
			}
		}
		turnover := math.NaN()
		if len(previousUniverse) > 0 {
			turnover = universeTurnoverPct(previousUniverse, assets)
		}
		concentration := 0.0
		if totalLiquidity > 0 {
			concentration = 100 * top5Liquidity / totalLiquidity
		}
		observations = append(observations, dailyBehaviourObservation{
			Date: date, Universe: assets, EntryQualified: float64(entryQualified), MedianRSI: median(rsis),
			HighRSISharePct: 100 * float64(highRSI) / float64(limit), UniverseTurnoverPct: turnover,
			LiquidityTop5SharePct: concentration,
		})
		previousUniverse = append([]string(nil), assets...)
	}
	return observations
}

func barsThroughDate(bars []sqlitemarket.Bar, date uint64) []sqlitemarket.Bar {
	index := sort.Search(len(bars), func(i int) bool { return bars[i].Date > date })
	return bars[:index]
}

func universeTurnoverPct(previous, current []string) float64 {
	if len(previous) == 0 || len(current) == 0 {
		return math.NaN()
	}
	set := make(map[string]struct{}, len(previous))
	for _, asset := range previous {
		set[asset] = struct{}{}
	}
	overlap := 0
	for _, asset := range current {
		if _, ok := set[asset]; ok {
			overlap++
		}
	}
	denominator := len(current)
	if len(previous) > denominator {
		denominator = len(previous)
	}
	return 100 * (1 - float64(overlap)/float64(denominator))
}

func median(values []float64) float64 {
	if len(values) == 0 {
		return math.NaN()
	}
	copyValues := append([]float64(nil), values...)
	sort.Float64s(copyValues)
	mid := len(copyValues) / 2
	if len(copyValues)%2 == 0 {
		return (copyValues[mid-1] + copyValues[mid]) / 2
	}
	return copyValues[mid]
}

func obsValues(observations []dailyBehaviourObservation, extract func(dailyBehaviourObservation) float64) []datedValue {
	values := make([]datedValue, 0, len(observations))
	for _, observation := range observations {
		value := extract(observation)
		if isFinite(value) {
			values = append(values, datedValue{Date: observation.Date, Value: value})
		}
	}
	return values
}

func obsValuesSkipNaN(observations []dailyBehaviourObservation, extract func(dailyBehaviourObservation) float64) []datedValue {
	return obsValues(observations, extract)
}

func buildBehaviourMetric(series behaviourSeries) (realBehaviourMetric, bool) {
	if len(series.Values) < 6 {
		return realBehaviourMetric{}, false
	}
	current := series.Values[len(series.Values)-1]
	baseline := series.Values[:len(series.Values)-1]
	numeric := make([]float64, 0, len(baseline))
	for _, value := range baseline {
		numeric = append(numeric, value.Value)
	}
	mean, std := meanStd(numeric)
	lower := percentile(numeric, 0.10)
	upper := percentile(numeric, 0.90)
	z := 0.0
	if std > 1e-9 {
		z = (current.Value - mean) / std
	}
	classification := classifyBehaviour(z)

	start := len(series.Values) - 12
	if start < 0 {
		start = 0
	}
	trend := make([]realBehaviourTrendPoint, 0, len(series.Values)-start)
	for _, point := range series.Values[start:] {
		trend = append(trend, realBehaviourTrendPoint{Label: shortMarketDate(point.Date), Live: point.Value, Mean: mean, Lower: lower, Upper: upper})
	}
	contrib := make([]realBehaviourContribution, 0, 3)
	for i := len(series.Values) - 1; i >= 0 && len(contrib) < 3; i-- {
		point := series.Values[i]
		pointZ := 0.0
		if std > 1e-9 {
			pointZ = (point.Value - mean) / std
		}
		contrib = append(contrib, realBehaviourContribution{
			Timestamp: formatMarketDate(point.Date), Asset: "UNIVERSE", CycleID: fmt.Sprintf("market:%d", point.Date),
			ValueLabel: series.Format(point.Value), DeviationLabel: signedSigma(pointZ),
		})
	}
	return realBehaviourMetric{
		ID: series.ID, Family: series.Family, Label: series.Label, CurrentLabel: series.Format(current.Value),
		HistoricalMeanLabel: series.Format(mean), HistoricalRangeLabel: fmt.Sprintf("%s–%s", series.Format(lower), series.Format(upper)),
		ZScore: round(z, 2), Classification: classification, Description: series.Description,
		Trend: trend, Distribution: histogram(numeric, 15), ContributingCycles: contrib,
	}, true
}

func meanStd(values []float64) (float64, float64) {
	if len(values) == 0 {
		return 0, 0
	}
	sum := 0.0
	for _, value := range values {
		sum += value
	}
	mean := sum / float64(len(values))
	if len(values) < 2 {
		return mean, 0
	}
	variance := 0.0
	for _, value := range values {
		delta := value - mean
		variance += delta * delta
	}
	return mean, math.Sqrt(variance / float64(len(values)-1))
}

func percentile(values []float64, q float64) float64 {
	if len(values) == 0 {
		return 0
	}
	copyValues := append([]float64(nil), values...)
	sort.Float64s(copyValues)
	if q <= 0 {
		return copyValues[0]
	}
	if q >= 1 {
		return copyValues[len(copyValues)-1]
	}
	position := q * float64(len(copyValues)-1)
	left := int(math.Floor(position))
	right := int(math.Ceil(position))
	if left == right {
		return copyValues[left]
	}
	fraction := position - float64(left)
	return copyValues[left]*(1-fraction) + copyValues[right]*fraction
}

func histogram(values []float64, bins int) []int {
	if bins <= 0 {
		return []int{}
	}
	result := make([]int, bins)
	if len(values) == 0 {
		return result
	}
	minValue, maxValue := values[0], values[0]
	for _, value := range values[1:] {
		if value < minValue {
			minValue = value
		}
		if value > maxValue {
			maxValue = value
		}
	}
	if math.Abs(maxValue-minValue) < 1e-12 {
		result[bins/2] = len(values)
		return result
	}
	for _, value := range values {
		index := int(math.Floor((value - minValue) / (maxValue - minValue) * float64(bins)))
		if index >= bins {
			index = bins - 1
		}
		if index < 0 {
			index = 0
		}
		result[index]++
	}
	return result
}

func classifyBehaviour(z float64) string {
	abs := math.Abs(z)
	switch {
	case abs < 1:
		return "NORMAL"
	case abs < 2:
		return "ELEVATED"
	case abs < 3:
		return "ABNORMAL"
	default:
		return "CRITICAL"
	}
}

func signedSigma(value float64) string {
	if value >= 0 {
		return fmt.Sprintf("+%.1fσ", value)
	}
	return fmt.Sprintf("%.1fσ", value)
}

func shortMarketDate(value uint64) string {
	date, ok := parseMarketDate(value)
	if !ok {
		return fmt.Sprintf("%d", value)
	}
	return date.Format("01-02")
}

func round(value float64, decimals int) float64 {
	factor := math.Pow10(decimals)
	return math.Round(value*factor) / factor
}
