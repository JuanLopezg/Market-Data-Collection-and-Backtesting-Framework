package provider

import (
	"strings"
	"testing"
	"time"

	pgstore "control-dashboard-api/internal/integration/postgres"
	"control-dashboard-api/internal/tradingwire"
)

func TestBuildLedgerStatusValidatesFillEconomics(t *testing.T) {
	window := pgstore.LedgerFillWindow{
		TotalRows: 2, DistinctFillIDs: 2, InvalidRows: 0, TotalCommission: 0.30,
		GrossBuyNotional: 100, GrossSellNotional: 120, LatestFillID: 2, LatestFillTimestamp: 20260926,
		Rows: []tradingwire.Fill{
			{FillID: 1, OrderID: 11, StrategyID: 7, Timestamp: 20260925, Coin: "BTCUSDT", Side: 0, Quantity: 1, Price: 100, Commission: 0.10},
			{FillID: 2, OrderID: 12, StrategyID: 7, Timestamp: 20260926, Coin: "BTCUSDT", Side: 1, Quantity: 1, Price: 120, Commission: 0.20},
		},
	}
	got := buildLedgerStatus(window, time.Unix(0, 0))
	if !got.Validated || !got.FoundationReady || got.Status != "VALIDATED" {
		t.Fatalf("ledger should validate: %+v", got)
	}
	if got.NetCashDeltaFromFills != 19.7 {
		t.Fatalf("net cash delta=%v, want 19.7", got.NetCashDeltaFromFills)
	}
	if len(got.Entries) != 2 || got.Entries[0].FillID != "2" || got.Entries[1].FillID != "1" {
		t.Fatalf("unexpected presentation order: %+v", got.Entries)
	}
	if got.Entries[0].CashDelta != 119.8 || got.Entries[0].PositionDelta != -1 {
		t.Fatalf("unexpected sell economics: %+v", got.Entries[0])
	}
	if len(got.RecentWindowFingerprint) != 64 || strings.Trim(got.RecentWindowFingerprint, "0") == "" {
		t.Fatalf("fingerprint not populated: %q", got.RecentWindowFingerprint)
	}
	if got.DurableRealizedPnL || got.DurableUnrealizedPnL || got.HistoricalEquityAvailable {
		t.Fatalf("foundation must not fabricate accounting/PnL availability")
	}
}

func TestBuildLedgerStatusBlocksDuplicateOrInvalidRows(t *testing.T) {
	duplicate := buildLedgerStatus(pgstore.LedgerFillWindow{TotalRows: 2, DistinctFillIDs: 1}, time.Now())
	if duplicate.Validated || !strings.Contains(duplicate.Error, "uniqueness") {
		t.Fatalf("duplicate fill ids must block: %+v", duplicate)
	}
	invalid := buildLedgerStatus(pgstore.LedgerFillWindow{TotalRows: 1, DistinctFillIDs: 1, InvalidRows: 1}, time.Now())
	if invalid.Validated || !strings.Contains(invalid.Error, "invalid economic") {
		t.Fatalf("invalid rows must block: %+v", invalid)
	}
}

func TestLedgerEntryRejectsMalformedFill(t *testing.T) {
	_, err := ledgerEntryFromFill(tradingwire.Fill{FillID: 1, OrderID: 1, StrategyID: 1, Timestamp: 1, Coin: "BTC", Side: 2, Quantity: 1, Price: 1}, strings.Repeat("0", 64))
	if err == nil {
		t.Fatal("unsupported side must fail")
	}
}
