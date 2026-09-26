package provider

import (
	"strings"
	"testing"
	"time"

	hldiag "control-dashboard-api/internal/integration/hyperliquid"
	registrydiag "control-dashboard-api/internal/integration/symbolregistry"
)

func syntheticRegistry() registrydiag.Registry {
	return registrydiag.Registry{
		SchemaVersion: 1, RegistryVersion: "test", Policy: "EXPLICIT_ONLY", MarketDataSource: "BINANCE",
		Entries: []registrydiag.Entry{
			{Internal: "BTCUSDT", MarketData: []registrydiag.VenueSymbol{{Venue: "BINANCE", Environment: "SOURCE", Symbol: "BTCUSDT", Status: "SUPPORTED"}}, Execution: []registrydiag.VenueSymbol{{Venue: "HYPERLIQUID", Environment: "TESTNET", Symbol: "BTC", Status: "MAPPED", RoutingPolicy: "ALLOW_IF_VENUE_PRESENT_AND_RULES_VALID"}}},
			{Internal: "XAIUSDT", MarketData: []registrydiag.VenueSymbol{{Venue: "BINANCE", Environment: "SOURCE", Symbol: "XAIUSDT", Status: "SUPPORTED"}}, Execution: []registrydiag.VenueSymbol{{Venue: "HYPERLIQUID", Environment: "TESTNET", Status: "BLOCKED_EXPLICIT", RoutingPolicy: "DENY", Reason: "unsupported"}}},
		},
	}
}

func syntheticManifest() hldiag.SymbolMapManifest {
	return hldiag.SymbolMapManifest{SchemaVersion: 2, Venue: "HYPERLIQUID", Environment: "TESTNET", MappingPolicy: "EXPLICIT_ONLY", Mappings: []hldiag.SymbolMapping{{Internal: "BTCUSDT", Venue: "BTC"}}, Unsupported: []hldiag.UnsupportedSymbol{{Internal: "XAIUSDT", Reason: "unsupported"}}}
}

func TestBuildSymbolRegistryStatusValidatedWithWarnings(t *testing.T) {
	result := buildSymbolRegistryStatus(syntheticRegistry(), "abc", syntheticManifest(), []string{"BTCUSDT", "XAIUSDT"}, []string{"BTCUSDT", "XAIUSDT"}, []string{"BTC"}, 5*time.Millisecond, time.Unix(0, 0))
	if !result.Validated || result.Status != "VALIDATED_WITH_WARNINGS" {
		t.Fatalf("unexpected validation: %+v", result)
	}
	if result.CurrentStrategyRoutableCount != 1 || result.CurrentStrategyNonRoutableCount != 1 {
		t.Fatalf("unexpected coverage: %+v", result)
	}
	if len(result.Alarms) != 1 || result.Alarms[0].EventType != "SYMBOL_COVERAGE_PARTIAL" {
		t.Fatalf("unexpected alarms: %+v", result.Alarms)
	}
}

func TestBuildSymbolRegistryStatusBlocksUnregisteredRanking(t *testing.T) {
	result := buildSymbolRegistryStatus(syntheticRegistry(), "abc", syntheticManifest(), []string{"BTCUSDT", "NEWUSDT"}, []string{"BTCUSDT"}, []string{"BTC"}, 0, time.Unix(0, 0))
	if result.Validated || result.Status != "BLOCKED" {
		t.Fatalf("expected blocked: %+v", result)
	}
	if len(result.UnregisteredRankingSymbols) != 1 || result.UnregisteredRankingSymbols[0] != "NEWUSDT" {
		t.Fatalf("unexpected unregistered ranking: %+v", result.UnregisteredRankingSymbols)
	}
	found := false
	for _, alarm := range result.Alarms {
		if alarm.EventType == "SYMBOL_UNREGISTERED_MARKET_DATA" && alarm.Asset == "NEWUSDT" {
			found = true
		}
	}
	if !found {
		t.Fatal("expected unregistered-market-data alarm")
	}
}

func TestRegistryManifestDivergenceIsCritical(t *testing.T) {
	reg := syntheticRegistry()
	reg.Entries[0].Execution[0].Symbol = "WBTC"
	result := buildSymbolRegistryStatus(reg, "abc", syntheticManifest(), []string{"BTCUSDT"}, []string{"BTCUSDT"}, []string{"BTC"}, 0, time.Unix(0, 0))
	if result.Validated || len(result.MappingDivergences) == 0 {
		t.Fatalf("expected divergence: %+v", result)
	}
	if !strings.Contains(result.MappingDivergences[0], "disagrees") {
		t.Fatalf("unexpected divergence detail: %v", result.MappingDivergences)
	}
}

func TestLive20260926RankingIsExplicitlyClassified(t *testing.T) {
	reg, _, err := registrydiag.Load()
	if err != nil {
		t.Fatal(err)
	}
	manifest, _, err := hldiag.ExplicitSymbolMap()
	if err != nil {
		t.Fatal(err)
	}
	ranking := []string{
		"1000PEPEUSDT", "2ZUSDT", "AAVEUSDT", "ADAUSDT", "AEROUSDT", "AKEUSDT", "ARBUSDT", "ARKUSDT", "AVAXUSDT", "BCHUSDT",
		"BNBUSDT", "BRUSDT", "BTCUSDT", "BTWUSDT", "DASHUSDT", "DOGEUSDT", "DOTUSDT", "ENAUSDT", "ETHUSDT", "FETUSDT",
		"FILUSDT", "HYPEUSDT", "INJUSDT", "LDOUSDT", "LINKUSDT", "LTCUSDT", "LYNUSDT", "MUBARAKUSDT", "NEARUSDT", "ONDOUSDT",
		"ONEUSDT", "PENGUUSDT", "PHAUSDT", "PUMPUSDT", "QNTUSDT", "QUSDT", "RAREUSDT", "SAGAUSDT", "SEIUSDT", "SOLUSDT",
		"SUIUSDT", "TAOUSDT", "TRUMPUSDT", "UNIUSDT", "WLDUSDT", "XLMUSDT", "XPLUSDT", "XRPUSDT", "ZECUSDT", "龙虾USDT",
	}
	strategy := []string{
		"1000PEPEUSDT", "ADAUSDT", "AKEUSDT", "ARBUSDT", "DOGEUSDT", "ENAUSDT", "MUBARAKUSDT", "ONDOUSDT", "ONEUSDT", "PENGUUSDT",
		"PHAUSDT", "PUMPUSDT", "SAGAUSDT", "SEIUSDT", "SUIUSDT", "WLDUSDT", "XLMUSDT", "XPLUSDT", "XRPUSDT", "龙虾USDT",
	}
	venueUniverse := []string{"kPEPE", "ADA", "ARB", "DOGE", "ONDO", "PENGU", "PUMP", "SUI", "WLD", "XLM", "XPL"}
	result := buildSymbolRegistryStatus(reg, "live-regression", manifest, ranking, strategy, venueUniverse, 0, time.Unix(0, 0))
	if !result.Validated || result.Status != "VALIDATED_WITH_WARNINGS" {
		t.Fatalf("expected validated-with-warnings, got: %+v", result)
	}
	if len(result.UnregisteredRankingSymbols) != 0 || len(result.UnregisteredStrategySymbols) != 0 {
		t.Fatalf("unexpected unregistered symbols: ranking=%v strategy=%v", result.UnregisteredRankingSymbols, result.UnregisteredStrategySymbols)
	}
	if result.CurrentRankingRegisteredCount != 50 || result.CurrentStrategyRegisteredCount != 20 {
		t.Fatalf("unexpected registry coverage: ranking=%d/50 strategy=%d/20", result.CurrentRankingRegisteredCount, result.CurrentStrategyRegisteredCount)
	}
	if result.CurrentStrategyRoutableCount != 11 || result.CurrentStrategyNonRoutableCount != 9 {
		t.Fatalf("unexpected routing coverage: routable=%d nonRoutable=%d", result.CurrentStrategyRoutableCount, result.CurrentStrategyNonRoutableCount)
	}
}
