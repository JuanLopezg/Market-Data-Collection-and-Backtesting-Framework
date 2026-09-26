package provider

import (
	"errors"
	"strings"
	"testing"
)

func TestBuildRealOverviewNeverInventsEquityOrReadiness(t *testing.T) {
	positions := realPositionsData{
		AccountCash:     "$100000.00",
		SourceUpdatedAt: "2026-09-25T07:00:00Z",
		Positions: []realPosition{{
			Asset: "BTCUSDT", Side: "Long", Quantity: 1, QuantityLabel: "1",
			EntryPriceLabel: "—", CurrentPriceLabel: "—", PnLUSDLabel: "—", PnLPctLabel: "—",
			Status: "UNKNOWN",
		}},
	}
	reconciliation := realReconciliationData{Status: "CLEAN", LastChecked: "2026-09-25 07:00:00 UTC", SourceNote: "current evidence"}
	execution := realExecutionData{OpenOrders: 2, SourceUpdatedAt: "2026-09-25T07:00:00Z"}
	risk := realRiskData{ApprovedGrossTargetLabel: "80.0%", ApprovedTargetNotionalLabel: "$80000.00", ActiveTargetCount: 8, DecisionTimestamp: "2026-09-25"}
	market := realMarketData{LatestCompletedCandle: "2026-09-24", HealthyAssets: 20, TotalAssets: 20, SignalCycleAligned: true}

	got := buildRealOverview(positions, reconciliation, nil, execution, nil, risk, nil, market, nil, nil)
	if got.EquityAvailable || got.LiveExpectedAvailable {
		t.Fatal("overview must not claim equity/history availability before canonical ledger/baseline wiring")
	}
	if got.Readiness != "DEGRADED" {
		t.Fatalf("readiness = %q, want DEGRADED until global readiness aggregation is wired", got.Readiness)
	}
	if got.ReconciliationStatus != "CLEAN" {
		t.Fatalf("reconciliation status = %q, want CLEAN", got.ReconciliationStatus)
	}
	if got.Stats[0].Value != "Not available" {
		t.Fatalf("total equity = %q, want fail-closed label", got.Stats[0].Value)
	}
	if got.Stats[1].Value != "$100000.00" {
		t.Fatalf("cash = %q", got.Stats[1].Value)
	}
	if got.MarketDataStatus != "ALIGNED" {
		t.Fatalf("market state = %q, want ALIGNED", got.MarketDataStatus)
	}
}

func TestBuildRealOverviewPausesOnCurrentBlockedReconciliation(t *testing.T) {
	positions := realPositionsData{AccountCash: "$1.00", SourceUpdatedAt: "2026-09-25T07:00:00Z"}
	reconciliation := realReconciliationData{Status: "BLOCKED", SourceNote: "current mismatch"}
	got := buildRealOverview(positions, reconciliation, nil, realExecutionData{}, nil, realRiskData{}, errors.New("risk unavailable"), realMarketData{}, errors.New("market unavailable"), []string{"degraded"})
	if got.Readiness != "PAUSED" {
		t.Fatalf("readiness = %q, want PAUSED", got.Readiness)
	}
	if !strings.Contains(got.ReadinessDetail, "BLOCKED") {
		t.Fatalf("readiness detail = %q", got.ReadinessDetail)
	}
	if got.Checks[0].State != "BLOCKED" {
		t.Fatalf("reconciliation check = %q, want BLOCKED", got.Checks[0].State)
	}
}

func TestOverviewMarketStateFailsClosedWhenCycleIsNotAligned(t *testing.T) {
	if got := overviewMarketState(realMarketData{SignalCycleAligned: false}); got != "PENDING" {
		t.Fatalf("market state = %q, want PENDING", got)
	}
	if got := overviewMarketState(realMarketData{SignalCycleAligned: true, StaleAssets: 0}); got != "ALIGNED" {
		t.Fatalf("market state = %q, want ALIGNED", got)
	}
}
