package hyperliquid

import "testing"

func TestExplicitSymbolMapIsExactVersionedAndClassifiesUnsupported(t *testing.T) {
	manifest, hash, err := ExplicitSymbolMap()
	if err != nil {
		t.Fatal(err)
	}
	if manifest.SchemaVersion != 2 || manifest.MappingPolicy != "EXPLICIT_ONLY" || manifest.Venue != "HYPERLIQUID" || manifest.Environment != "TESTNET" {
		t.Fatalf("unexpected manifest: %+v", manifest)
	}
	if len(hash) != 64 {
		t.Fatalf("unexpected sha256 %q", hash)
	}
	idx := MappingIndex(manifest)
	if idx["BTCUSDT"] != "BTC" || idx["ETHUSDT"] != "ETH" {
		t.Fatalf("missing exact core mappings: %#v", idx)
	}
	if idx["1000PEPEUSDT"] != "kPEPE" || idx["1000BONKUSDT"] != "kBONK" {
		t.Fatalf("missing explicit alias mappings")
	}
	if _, ok := idx["BTC"]; ok {
		t.Fatalf("internal mapping must not silently accept base symbols")
	}
	unsupported := UnsupportedIndex(manifest)
	for _, symbol := range []string{"AKEUSDT", "BROCCOLI714USDT", "MARSCOINUSDT", "NILUSDT", "NOMUSDT", "SAGAUSDT", "SOONUSDT", "USUSDT", "XAIUSDT", "龙虾USDT"} {
		if unsupported[symbol] == "" {
			t.Fatalf("expected explicit unsupported classification for %q", symbol)
		}
	}
}
