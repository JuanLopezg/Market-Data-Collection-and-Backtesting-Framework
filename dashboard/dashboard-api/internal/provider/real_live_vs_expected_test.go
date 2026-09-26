package provider

import (
	"strings"
	"testing"
	"time"

	sqlitemarket "control-dashboard-api/internal/integration/sqlite"
)

func TestBuildRealLiveVsExpectedUsesCanonicalRollingObservations(t *testing.T) {
	start, _ := time.Parse("20060102", "20260701")
	var bars []sqlitemarket.Bar
	var ranking []sqlitemarket.DailyRankingRow
	for i := 0; i < 70; i++ {
		date := uint64(mustDateInt(start.AddDate(0, 0, i)))
		for assetIndex, pair := range []string{"AAAUSDT", "BBBUSDT", "CCCUSDT"} {
			base := 100.0 + float64(assetIndex*20)
			closeValue := base + float64(i)*(1.0+float64(assetIndex)*0.1)
			bars = append(bars, sqlitemarket.Bar{Pair: pair, Date: date, Open: closeValue - 1, High: closeValue + 1, Low: closeValue - 2, Close: closeValue, Volume: 1000 + float64((3-assetIndex)*100) + float64(i)})
			if i >= 30 {
				ranking = append(ranking, sqlitemarket.DailyRankingRow{Date: date, Rank: assetIndex + 1, Pair: pair, QuoteVolume: 10000 - float64(assetIndex*100)})
			}
		}
	}
	window := sqlitemarket.BehaviourWindow{
		LatestDate: uint64(mustDateInt(start.AddDate(0, 0, 69))),
		StartDate:  uint64(mustDateInt(start.AddDate(0, 0, 30))),
		WarmupDate: uint64(mustDateInt(start)),
		Ranking:    ranking,
		Bars:       bars,
	}
	data := buildRealLiveVsExpected(window, 50, 3)
	if data.SourceMode != "REAL" || data.MetricCount != 5 || len(data.Metrics) != 5 {
		t.Fatalf("unexpected baseline summary: %+v", data)
	}
	if data.BaselineLabel != "Canonical SQLite rolling baseline" || data.LatestObservation == "" {
		t.Fatalf("unexpected baseline metadata: %+v", data)
	}
	for _, metric := range data.Metrics {
		if len(metric.Trend) == 0 || len(metric.Distribution) != 15 || metric.CurrentLabel == "" {
			t.Fatalf("metric is incomplete: %+v", metric)
		}
	}
}

func TestStep45InsufficientHistoryStillValidatesContractWithoutInventingProjection(t *testing.T) {
	start, _ := time.Parse("20060102", "20260923")
	var bars []sqlitemarket.Bar
	var ranking []sqlitemarket.DailyRankingRow
	for i := 0; i < 35; i++ {
		date := uint64(mustDateInt(start.AddDate(0, 0, i)))
		for assetIndex, pair := range []string{"AAAUSDT", "BBBUSDT", "CCCUSDT"} {
			closeValue := 100.0 + float64(assetIndex*20) + float64(i)
			bars = append(bars, sqlitemarket.Bar{Pair: pair, Date: date, Open: closeValue - 1, High: closeValue + 1, Low: closeValue - 2, Close: closeValue, Volume: 1000 + float64((3-assetIndex)*100) + float64(i)})
			if i >= 32 { // exactly three completed ranking observations
				ranking = append(ranking, sqlitemarket.DailyRankingRow{Date: date, Rank: assetIndex + 1, Pair: pair, QuoteVolume: 10000 - float64(assetIndex*100)})
			}
		}
	}
	window := sqlitemarket.BehaviourWindow{
		LatestDate: uint64(mustDateInt(start.AddDate(0, 0, 34))),
		StartDate:  uint64(mustDateInt(start.AddDate(0, 0, 32))),
		WarmupDate: uint64(mustDateInt(start)),
		Ranking:    ranking,
		Bars:       bars,
	}
	data := buildRealLiveVsExpected(window, 50, 3)
	if data.ContractVersion != "step45-v1" || data.Status != "VALIDATED_LIMITED" || !data.Validated || data.ProjectionReady {
		t.Fatalf("insufficient history must validate the contract but keep projection unready: %+v", data)
	}
	if data.OverallClassification != "INSUFFICIENT_DATA" || data.MetricCount != 0 || len(data.Metrics) != 0 || len(data.Anomalies) != 0 {
		t.Fatalf("insufficient history must not fabricate classifications: %+v", data)
	}
	if data.BaselineObservationCount != 2 || len(data.BaselineFingerprint) != 64 || !data.BaselineExcludesLatest {
		t.Fatalf("insufficient-history baseline identity is incomplete: %+v", data)
	}
	if len(data.Coverage) != 4 || data.Coverage[0].State != "INSUFFICIENT_DATA" {
		t.Fatalf("market coverage must disclose insufficient history explicitly: %+v", data.Coverage)
	}
	for _, term := range []string{"PnL", "slippage", "replay"} {
		if !strings.Contains(strings.ToLower(data.SourceNote), strings.ToLower(term)) {
			t.Fatalf("insufficient-history source note must disclose deferred %s baseline: %q", term, data.SourceNote)
		}
	}
	if !data.ReadOnly || data.PrivateAuth != "DEFERRED" || data.OrderRouting != "DISABLED" {
		t.Fatalf("step45 safety boundary changed under insufficient history: %+v", data)
	}
}

func TestBehaviourClassificationUsesAbsoluteZScore(t *testing.T) {
	cases := map[float64]string{0.2: "NORMAL", -1.2: "ELEVATED", 2.2: "ABNORMAL", -3.1: "CRITICAL"}
	for value, expected := range cases {
		if actual := classifyBehaviour(value); actual != expected {
			t.Fatalf("z=%v expected %s got %s", value, expected, actual)
		}
	}
}

func TestStep45ProjectionContractIsVersionedReadOnlyAndLimited(t *testing.T) {
	start, _ := time.Parse("20060102", "20260701")
	var bars []sqlitemarket.Bar
	var ranking []sqlitemarket.DailyRankingRow
	for i := 0; i < 70; i++ {
		date := uint64(mustDateInt(start.AddDate(0, 0, i)))
		for assetIndex, pair := range []string{"AAAUSDT", "BBBUSDT", "CCCUSDT"} {
			closeValue := 100.0 + float64(assetIndex*20) + float64(i)
			bars = append(bars, sqlitemarket.Bar{Pair: pair, Date: date, Open: closeValue - 1, High: closeValue + 1, Low: closeValue - 2, Close: closeValue, Volume: 1000 + float64((3-assetIndex)*100) + float64(i)})
			if i >= 30 {
				ranking = append(ranking, sqlitemarket.DailyRankingRow{Date: date, Rank: assetIndex + 1, Pair: pair, QuoteVolume: 10000 - float64(assetIndex*100)})
			}
		}
	}
	window := sqlitemarket.BehaviourWindow{LatestDate: uint64(mustDateInt(start.AddDate(0, 0, 69))), StartDate: uint64(mustDateInt(start.AddDate(0, 0, 30))), WarmupDate: uint64(mustDateInt(start)), Ranking: ranking, Bars: bars}
	data := buildRealLiveVsExpected(window, 50, 3)
	if data.ContractVersion != "step45-v1" || data.Status != "VALIDATED_LIMITED" || !data.Validated || !data.ProjectionReady {
		t.Fatalf("unexpected step45 status: %+v", data)
	}
	if len(data.BaselineFingerprint) != 64 || data.BaselineObservationCount < 5 || !data.BaselineExcludesLatest {
		t.Fatalf("baseline identity is incomplete: %+v", data)
	}
	if !data.ReadOnly || data.PrivateAuth != "DEFERRED" || data.OrderRouting != "DISABLED" {
		t.Fatalf("step45 safety boundary changed: %+v", data)
	}
	if len(data.Coverage) != 4 || data.Coverage[0].State != "VALIDATED" {
		t.Fatalf("unexpected coverage: %+v", data.Coverage)
	}
	for _, row := range data.Coverage[1:] {
		if row.State != "DEFERRED" {
			t.Fatalf("non-market baseline must remain deferred: %+v", row)
		}
	}
}

func TestStep45BaselineFingerprintIsDeterministicAndConfigBound(t *testing.T) {
	obs := []dailyBehaviourObservation{
		{Date: 20260920, Universe: []string{"AAA", "BBB"}, EntryQualified: 1, MedianRSI: 72.5, HighRSISharePct: 50, UniverseTurnoverPct: 10, LiquidityTop5SharePct: 80},
		{Date: 20260921, Universe: []string{"AAA", "CCC"}, EntryQualified: 2, MedianRSI: 79.1, HighRSISharePct: 100, UniverseTurnoverPct: 50, LiquidityTop5SharePct: 75},
	}
	a := behaviourBaselineFingerprint(obs, 50, 20)
	b := behaviourBaselineFingerprint(obs, 50, 20)
	c := behaviourBaselineFingerprint(obs, 100, 20)
	if a != b || a == c || len(a) != 64 {
		t.Fatalf("fingerprint is not deterministic/config-bound: %q %q %q", a, b, c)
	}
}
