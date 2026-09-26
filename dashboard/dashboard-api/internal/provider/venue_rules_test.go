package provider

import (
	hldiag "control-dashboard-api/internal/integration/hyperliquid"
	"testing"
)

func TestBuildVenueTradingRulesValidatesOnlySupportedMappings(t *testing.T) {
	mapping := VenueSymbolMappingStatus{
		Status: "VALIDATED_WITH_UNSUPPORTED", Validated: true, Venue: "HYPERLIQUID", TargetEnvironment: "TESTNET",
		MappedRequiredCount: 2, UnsupportedRequiredCount: 1,
		Rows: []VenueSymbolMappingRow{
			{Internal: "BTCUSDT", Venue: "BTC", State: "VALID"},
			{Internal: "DOGEUSDT", Venue: "DOGE", State: "VALID"},
			{Internal: "NOPEUSDT", State: "UNSUPPORTED_EXPLICIT"},
		},
	}
	got := buildVenueTradingRules(mapping, []hldiag.PublicPerpRule{
		{Name: "BTC", SzDecimals: 5, MaxLeverage: 50, Mid: "100000"},
		{Name: "DOGE", SzDecimals: 0, MaxLeverage: 20, Mid: "0.25", OnlyIsolated: true, MarginMode: "noCross"},
	})
	if !got.Validated || got.Status != "VALIDATED" || got.ValidatedRuleCount != 2 || got.BlockedRuleCount != 0 {
		t.Fatalf("unexpected result: %+v", got)
	}
	if len(got.Rows) != 2 {
		t.Fatalf("unsupported mappings must not become rule rows: %+v", got.Rows)
	}
	if got.Rows[0].Internal != "BTCUSDT" || got.Rows[0].SizeStepLabel != "0.00001" || got.Rows[0].PriceMaxDecimals != 1 || got.Rows[0].EstimatedMinOrderSizeLabel != "0.00010" {
		t.Fatalf("unexpected BTC rule: %+v", got.Rows[0])
	}
	if got.Rows[1].Internal != "DOGEUSDT" || got.Rows[1].SizeStepLabel != "1" || got.Rows[1].PriceMaxDecimals != 6 || got.Rows[1].EstimatedMinOrderSizeLabel != "40" {
		t.Fatalf("unexpected DOGE rule: %+v", got.Rows[1])
	}
	if got.NonRoutableMappingCount != 1 {
		t.Fatalf("unsupported mapping count lost: %+v", got)
	}
}

func TestBuildVenueTradingRulesBlocksDelistedOrMissingMid(t *testing.T) {
	mapping := VenueSymbolMappingStatus{Validated: true, Venue: "HYPERLIQUID", TargetEnvironment: "TESTNET", MappedRequiredCount: 2, Rows: []VenueSymbolMappingRow{
		{Internal: "AAAUSDT", Venue: "AAA", State: "VALID"},
		{Internal: "BBBUSDT", Venue: "BBB", State: "VALID"},
	}}
	got := buildVenueTradingRules(mapping, []hldiag.PublicPerpRule{
		{Name: "AAA", SzDecimals: 2, MaxLeverage: 10, Mid: "1.23", IsDelisted: true},
		{Name: "BBB", SzDecimals: 2, MaxLeverage: 10, Mid: ""},
	})
	if got.Validated || got.Status != "BLOCKED" || got.BlockedRuleCount != 2 {
		t.Fatalf("invalid public rules must fail closed: %+v", got)
	}
}

func TestBuildVenueTradingRulesBlocksInvalidSzDecimals(t *testing.T) {
	mapping := VenueSymbolMappingStatus{Validated: true, Venue: "HYPERLIQUID", TargetEnvironment: "TESTNET", MappedRequiredCount: 1, Rows: []VenueSymbolMappingRow{{Internal: "AAAUSDT", Venue: "AAA", State: "VALID"}}}
	got := buildVenueTradingRules(mapping, []hldiag.PublicPerpRule{{Name: "AAA", SzDecimals: 7, MaxLeverage: 10, Mid: "1"}})
	if got.Validated || got.BlockedRuleCount != 1 {
		t.Fatalf("szDecimals > 6 must fail closed: %+v", got)
	}
}
