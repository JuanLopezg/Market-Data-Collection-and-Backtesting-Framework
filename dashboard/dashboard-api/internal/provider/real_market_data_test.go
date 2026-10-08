package provider

import (
	"math"
	"testing"
	"time"

	pgstore "control-dashboard-api/internal/integration/postgres"
	sqlitemarket "control-dashboard-api/internal/integration/sqlite"
	"control-dashboard-api/internal/tradingwire"
)

func TestQuoteLiquidityUsesTradedNotionalAndPreservesOldCycle(t *testing.T) {
	btc := makeTrendBars("BTCUSDT", 20260821, 30)
	pepe := makeTrendBars("PEPEUSDT", 20260821, 30)
	for i := range btc {
		btc[i].Volume = 10
		pepe[i].Volume = 1e12
		btcQuote, pepeQuote := 1e9, 1e6
		btc[i].QuoteVolume = &btcQuote
		pepe[i].QuoteVolume = &pepeQuote
	}
	window := sqlitemarket.StrategyWindow{LatestDate: 20260919,
		Ranking: []sqlitemarket.RankingRow{{Rank: 1, Pair: "BTCUSDT"}, {Rank: 2, Pair: "PEPEUSDT"}},
		Bars:    append(btc, pepe...)}
	checkpoint := pgstore.StrategyCheckpoint{Timestamp: 20260919,
		Update:  tradingwire.MarketDataUpdated{CompletedThrough: 20260919, ActiveTopN: 50},
		Intents: tradingwire.StrategyIntentBatch{Timestamp: 20260919}}
	base := buildRealMarketData(window, checkpoint, nil, 50, 100, 1)
	quote := buildRealMarketData(window, checkpoint, nil, 50, 100, 1,
		RealConfig{QuoteVolume: true, QuoteVolumeFrom: "20260920"})
	if base.Universe[0].Asset != "PEPEUSDT" || quote.Universe[0].Asset != "BTCUSDT" || quote.Universe[0].SMAVolumeLabel != "1.00B" {
		t.Fatalf("ranking must use actual quote turnover: base=%+v quote=%+v", base.Universe, quote.Universe)
	}
	if quote.SignalCycleAligned || quote.LiquidityLabel != "SMA Quote Volume 25 (USDT)" {
		t.Fatalf("old cycle must not be relabeled as a quote-volume decision: %+v", quote)
	}
	quote = buildRealMarketData(window, checkpoint, nil, 50, 100, 1,
		RealConfig{QuoteVolume: true, QuoteVolumeFrom: "20260919"})
	if !quote.SignalCycleAligned {
		t.Fatal("new metric cycle should align")
	}
	btc[29].QuoteVolume = nil
	window.Bars = append(btc, pepe...)
	quote = buildRealMarketData(window, checkpoint, nil, 50, 100, 1, RealConfig{QuoteVolume: true})
	if quote.Universe[0].Asset != "PEPEUSDT" || !math.IsNaN(liquidityValue(btc[29], true)) || quote.Integrity.InvalidRows != 1 {
		t.Fatal("missing quote turnover must never fall back to base units or volume * close")
	}
}

func TestBuildRealMarketDataRecomputesVerifiedIndicatorsAndJoinsOnlyAlignedSignals(t *testing.T) {
	bars := make([]sqlitemarket.Bar, 0, 60)
	start, _ := time.Parse("20060102", "20260821")
	for i := 0; i < 30; i++ {
		date := uint64(mustDateInt(start.AddDate(0, 0, i)))
		bars = append(bars,
			sqlitemarket.Bar{Pair: "BTCUSDT", Date: date, Open: 100 + float64(i), High: 102 + float64(i), Low: 99 + float64(i), Close: 101 + float64(i), Volume: 1000 + float64(i)},
			sqlitemarket.Bar{Pair: "ETHUSDT", Date: date, Open: 200 + float64(i), High: 202 + float64(i), Low: 199 + float64(i), Close: 201 + float64(i), Volume: 500 + float64(i)},
		)
	}
	window := sqlitemarket.StrategyWindow{
		LatestDate: 20260919,
		StartDate:  20260612,
		Ranking: []sqlitemarket.RankingRow{
			{Rank: 1, Pair: "BTCUSDT", QuoteVolume: 100000},
			{Rank: 2, Pair: "ETHUSDT", QuoteVolume: 90000},
		},
		Bars:      bars,
		Integrity: sqlitemarket.Integrity{},
	}
	checkpoint := pgstore.StrategyCheckpoint{
		Timestamp: 20260919,
		Update:    tradingwire.MarketDataUpdated{CompletedThrough: 20260919, Source: "binance", ActiveTopN: 50},
		Intents: tradingwire.StrategyIntentBatch{Timestamp: 20260919, Strategies: []tradingwire.StrategySignalIntent{{
			StrategyID: 1, StrategyName: "Pure_RSI", Signals: map[string]float64{"BTCUSDT": 1},
		}}},
	}
	data := buildRealMarketData(window, checkpoint, nil, 50, 100, 2)
	if data.SourceMode != "REAL" || !data.SignalCycleAligned || data.ActiveSignals != 1 || data.AvailableSlots != 9 {
		t.Fatalf("unexpected real market summary: %+v", data)
	}
	if len(data.Universe) != 2 || data.Universe[0].Asset != "BTCUSDT" || data.Universe[0].Signal != "LONG" || data.Universe[0].RSI != 100 {
		t.Fatalf("unexpected universe: %+v", data.Universe)
	}
	if data.Integrity.MissingCandles != 0 || data.StaleAssets != 0 || data.HealthyAssets != 2 {
		t.Fatalf("unexpected integrity/freshness: %+v", data)
	}
	if len(data.CandidateRejections) != 1 || data.CandidateRejections[0].Asset != "ETHUSDT" {
		t.Fatalf("expected aligned signal mismatch diagnostic for ETH: %+v", data.CandidateRejections)
	}
}

func TestBuildRealMarketDataDoesNotMixUnalignedStrategyCycle(t *testing.T) {
	window := sqlitemarket.StrategyWindow{
		LatestDate: 20260919,
		Ranking:    []sqlitemarket.RankingRow{{Rank: 1, Pair: "BTCUSDT"}},
		Bars:       makeTrendBars("BTCUSDT", 20260821, 30),
	}
	checkpoint := pgstore.StrategyCheckpoint{Timestamp: 20260918, Update: tradingwire.MarketDataUpdated{CompletedThrough: 20260918}, Intents: tradingwire.StrategyIntentBatch{Timestamp: 20260918}}
	data := buildRealMarketData(window, checkpoint, nil, 50, 100, 1)
	if data.SignalCycleAligned || data.Universe[0].SignalState != "NOT ALIGNED" || data.ActiveSignals != 0 {
		t.Fatalf("unaligned cycle must not be mixed: %+v", data)
	}
}

func makeTrendBars(pair string, startDate int, count int) []sqlitemarket.Bar {
	start, _ := time.Parse("20060102", formatIntDate(startDate))
	result := make([]sqlitemarket.Bar, 0, count)
	for i := 0; i < count; i++ {
		date := uint64(mustDateInt(start.AddDate(0, 0, i)))
		result = append(result, sqlitemarket.Bar{Pair: pair, Date: date, Open: 10 + float64(i), High: 12 + float64(i), Low: 9 + float64(i), Close: 11 + float64(i), Volume: 100 + float64(i)})
	}
	return result
}

func mustDateInt(value time.Time) int {
	return value.Year()*10000 + int(value.Month())*100 + value.Day()
}

func formatIntDate(value int) string {
	return time.Date(value/10000, time.Month((value/100)%100), value%100, 0, 0, 0, 0, time.UTC).Format("20060102")
}
