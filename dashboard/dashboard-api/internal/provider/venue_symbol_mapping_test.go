package provider

import (
	hldiag "control-dashboard-api/internal/integration/hyperliquid"
	"testing"
)

func TestBuildVenueSymbolMappingRequiresExactCoverage(t *testing.T) {
	manifest := hldiag.SymbolMapManifest{SchemaVersion: 2, Venue: "HYPERLIQUID", Environment: "TESTNET", MappingPolicy: "EXPLICIT_ONLY", Mappings: []hldiag.SymbolMapping{{Internal: "BTCUSDT", Venue: "BTC"}, {Internal: "1000PEPEUSDT", Venue: "kPEPE"}}}
	got := buildVenueSymbolMapping([]string{"BTCUSDT", "1000PEPEUSDT"}, []string{"BTC", "kPEPE"}, manifest, "abc")
	if !got.Validated || got.Status != "VALIDATED" || got.MappedRequiredCount != 2 || got.ClassifiedRequiredCount != 2 || !got.ExecutionCoverageComplete {
		t.Fatalf("unexpected validated mapping: %+v", got)
	}
}

func TestBuildVenueSymbolMappingAcceptsExplicitUnsupportedButKeepsExecutionCoverageClosed(t *testing.T) {
	manifest := hldiag.SymbolMapManifest{SchemaVersion: 2, Venue: "HYPERLIQUID", Environment: "TESTNET", MappingPolicy: "EXPLICIT_ONLY", Mappings: []hldiag.SymbolMapping{{Internal: "BTCUSDT", Venue: "BTC"}, {Internal: "ETHUSDT", Venue: "ETH"}}, Unsupported: []hldiag.UnsupportedSymbol{{Internal: "SOLUSDT", Reason: "No approved TESTNET mapping."}}}
	got := buildVenueSymbolMapping([]string{"BTCUSDT", "ETHUSDT", "SOLUSDT"}, []string{"BTC"}, manifest, "abc")
	if !got.Validated || got.Status != "VALIDATED_WITH_UNSUPPORTED" || got.ExecutionCoverageComplete {
		t.Fatalf("classification should validate while execution coverage stays closed: %+v", got)
	}
	if got.MappedRequiredCount != 1 || got.UnsupportedRequiredCount != 2 || got.ClassifiedRequiredCount != 3 {
		t.Fatalf("unexpected coverage counts: %+v", got)
	}
	if len(got.MissingInternalSymbols) != 0 {
		t.Fatalf("explicit unsupported symbol must not be missing: %+v", got)
	}
	if len(got.MissingVenueSymbols) != 1 || got.MissingVenueSymbols[0] != "ETH" {
		t.Fatalf("venue-absent mapping not reported: %+v", got)
	}
}

func TestBuildVenueSymbolMappingStillFailsClosedOnUnclassifiedInternalSymbol(t *testing.T) {
	manifest := hldiag.SymbolMapManifest{SchemaVersion: 2, Venue: "HYPERLIQUID", Environment: "TESTNET", MappingPolicy: "EXPLICIT_ONLY", Mappings: []hldiag.SymbolMapping{{Internal: "BTCUSDT", Venue: "BTC"}}}
	got := buildVenueSymbolMapping([]string{"BTCUSDT", "SOLUSDT"}, []string{"BTC"}, manifest, "abc")
	if got.Validated || got.Status != "BLOCKED" {
		t.Fatalf("unclassified symbol must fail closed: %+v", got)
	}
	if len(got.MissingInternalSymbols) != 1 || got.MissingInternalSymbols[0] != "SOLUSDT" {
		t.Fatalf("missing internal not reported: %+v", got)
	}
}

func TestBuildVenueSymbolMappingRegressionCurrentTop20WithUnsupported(t *testing.T) {
	manifest, hash, err := hldiag.ExplicitSymbolMap()
	if err != nil {
		t.Fatal(err)
	}
	required := []string{
		"1000PEPEUSDT", "ADAUSDT", "AKEUSDT", "ARBUSDT", "BROCCOLI714USDT",
		"DOGEUSDT", "ENAUSDT", "LSKUSDT", "NILUSDT", "NOMUSDT", "ONEUSDT",
		"PENGUUSDT", "PUMPUSDT", "SAGAUSDT", "WLDUSDT", "XAIUSDT", "XLMUSDT",
		"XPLUSDT", "XRPUSDT", "龙虾USDT",
	}
	// Mirrors the observed TESTNET coverage from the failed Step 35 run:
	// ENA/LSK/ONE/XRP are mapped but absent; seven additional internals are
	// explicitly classified unsupported in the manifest.
	venueUniverse := []string{"kPEPE", "ADA", "ARB", "DOGE", "PENGU", "PUMP", "WLD", "XLM", "XPL"}
	got := buildVenueSymbolMapping(required, venueUniverse, manifest, hash)
	if !got.Validated || got.Status != "VALIDATED_WITH_UNSUPPORTED" {
		t.Fatalf("current universe should be fully classified, got: %+v", got)
	}
	if got.RequiredSymbolCount != 20 || got.ClassifiedRequiredCount != 20 || got.MappedRequiredCount != 9 || got.UnsupportedRequiredCount != 11 {
		t.Fatalf("unexpected current-universe counts: %+v", got)
	}
	if got.ExecutionCoverageComplete {
		t.Fatalf("execution coverage must remain incomplete while 11 symbols are unsupported")
	}
	if len(got.MissingInternalSymbols) != 0 {
		t.Fatalf("no current symbol should remain unclassified: %+v", got.MissingInternalSymbols)
	}
}
