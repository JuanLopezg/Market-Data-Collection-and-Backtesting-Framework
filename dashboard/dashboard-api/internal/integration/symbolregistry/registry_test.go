package symbolregistry

import "testing"

func TestRegistryLoadsAndIsExplicit(t *testing.T) {
	registry, hash, err := Load()
	if err != nil {
		t.Fatal(err)
	}
	if hash == "" {
		t.Fatal("expected registry hash")
	}
	if len(registry.Entries) != 175 {
		t.Fatalf("entries=%d want=175", len(registry.Entries))
	}
	idx := EntryIndex(registry)
	pepe, ok := idx["1000PEPEUSDT"]
	if !ok {
		t.Fatal("1000PEPEUSDT missing")
	}
	binance, ok := FindLeg(pepe.MarketData, "BINANCE", "SOURCE")
	if !ok || binance.Symbol != "1000PEPEUSDT" {
		t.Fatalf("unexpected Binance leg: %+v", binance)
	}
	hl, ok := FindLeg(pepe.Execution, "HYPERLIQUID", "TESTNET")
	if !ok || hl.Symbol != "kPEPE" || hl.Status != "MAPPED" {
		t.Fatalf("unexpected Hyperliquid leg: %+v", hl)
	}
	blocked := idx["AKEUSDT"]
	leg, ok := FindLeg(blocked.Execution, "HYPERLIQUID", "TESTNET")
	if !ok || leg.Status != "BLOCKED_EXPLICIT" || leg.RoutingPolicy != "DENY" {
		t.Fatalf("unexpected blocked leg: %+v", leg)
	}
	for _, symbol := range []string{"MARSCOINUSDT", "SOONUSDT", "USUSDT"} {
		entry, ok := idx[symbol]
		if !ok {
			t.Fatalf("%s missing", symbol)
		}
		leg, ok := FindLeg(entry.Execution, "HYPERLIQUID", "TESTNET")
		if !ok || leg.Status != "BLOCKED_EXPLICIT" || leg.RoutingPolicy != "DENY" || leg.Symbol != "" {
			t.Fatalf("unexpected explicit block for %s: %+v", symbol, leg)
		}
	}
}
