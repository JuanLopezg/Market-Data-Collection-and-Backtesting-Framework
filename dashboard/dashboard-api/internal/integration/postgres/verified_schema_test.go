package postgres

import "testing"

func TestAuditedRuntimeTablesAndSnapshotKeys(t *testing.T) {
	if TableTradingRuntimeState != "trading_runtime_state" || TableTradingFills != "trading_fills" {
		t.Fatal("execution-state table mapping changed")
	}
	if len(TradingRuntimeSnapshotKeys) == 0 {
		t.Fatal("runtime snapshot key list is empty")
	}
	seen := map[string]bool{}
	for _, key := range TradingRuntimeSnapshotKeys {
		if key == "" {
			t.Fatal("runtime snapshot contains an empty key")
		}
		if seen[key] {
			t.Fatalf("duplicate runtime snapshot key: %s", key)
		}
		seen[key] = true
	}
}
