package provider

import (
	"context"
	"encoding/json"
	"math"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestSimulationSlippageDailySum(t *testing.T) {
	orders := map[string]simulationOrder{"order-1": {Side: "BUY", LimitPrice: 100}}
	fills := []simulationFill{
		{OrderID: "order-1", Timestamp: 202610189, Quantity: 1, Price: 100.10},
		{OrderID: "order-1", Timestamp: 202610189, Quantity: 3, Price: 99.96},
		{OrderID: "order-1", Timestamp: 202610199, Quantity: 1, Price: 100.02},
	}
	got := simulationSlippage(fills, orders)
	if !got.available || len(got.daily) != 2 {
		t.Fatalf("unexpected fill coverage: %+v", got)
	}
	if got.daily[0]["label"] != "2026-10-18" || got.daily[0]["fills"] != 2 || math.Abs(got.daily[0]["bps"].(float64)-6) > 1e-8 {
		t.Fatalf("10 and -4 bps must sum to 6 regardless of notional: %+v", got.daily)
	}
	if got.daily[1]["label"] != "2026-10-19" || got.daily[1]["fills"] != 1 || math.Abs(got.daily[1]["bps"].(float64)-2) > 1e-8 {
		t.Fatalf("fills from another day must stay separate: %+v", got.daily)
	}
	if got.averageLabel != "0.00 bps" {
		t.Fatalf("overall average must remain notional weighted: %+v", got)
	}
	if got.bestLabel != "-4.00 bps" || got.worstLabel != "+10.00 bps" {
		t.Fatalf("overall best and worst must remain per fill: %+v", got)
	}
}

func writeSimulationFixture(t *testing.T, dir string) simulationSnapshot {
	t.Helper()
	s := simulationSnapshot{
		SchemaVersion:            1,
		Generation:               7,
		Phase:                    "MANUAL_READY",
		HistoricalDate:           "2020-01-02",
		BusinessTimestamp:        202001029,
		DayIndex:                 2,
		DayCount:                 2,
		SourceRowsProcessed:      10,
		VenueID:                  "MOCK",
		Environment:              "MOCK",
		ReplaySpeed:              1000,
		RouteSafe:                true,
		Reconciliation:           simulationReconciliation{State: "CLEAN", LocalSequence: 4, VenueSequence: 4, LedgerHeadHash: strings.Repeat("a", 64), Issues: []simulationReconciliationIssue{}},
		Account:                  simulationAccount{Equity: 100000, MarginUsed: 0, CashTotal: 100000, CashAvailable: 100000, Positions: []simulationPosition{}},
		Orders:                   []simulationOrder{},
		Fills:                    []simulationFill{},
		Accounting:               []simulationAccounting{},
		Market:                   []simulationMarketBar{{Asset: "BTCUSDT", Open: 100, High: 101, Low: 99, Close: 100, Volume: 1000}},
		EquityHistory:            []simulationEquityPoint{{Label: "2020-01-02", Equity: 100000}},
		Ledger:                   simulationLedger{Valid: true, Error: "", HeadHash: strings.Repeat("a", 64), Entries: []simulationLedgerEntry{}},
		RoutableAssets:           []string{"BTCUSDT"},
		Manual:                   simulationManualState{Ready: true, DecisionTimestamp: 202001039, ExecutionTimestamp: 202001040},
		EconomicFingerprint:      "1234567890abcdef",
		StreamFingerprint:        "fedcba0987654321",
		Step56RuntimeFingerprint: strings.Repeat("b", 64),
		Step57ManualFingerprint:  strings.Repeat("c", 64),
	}
	raw, err := json.Marshal(s)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "state.json"), raw, 0o600); err != nil {
		t.Fatal(err)
	}
	return s
}

func TestSimulationProviderReadsCanonicalSnapshot(t *testing.T) {
	dir := t.TempDir()
	writeSimulationFixture(t, dir)
	p := NewSimulation(SimulationConfig{Dir: dir})

	h := p.Health(context.Background())
	if !h.Ready || h.Mode != "mock" {
		t.Fatalf("unexpected health: %+v", h)
	}

	for _, resource := range AllResources {
		raw, err := p.Read(context.Background(), resource)
		if err != nil {
			t.Fatalf("read %s: %v", resource, err)
		}
		if !json.Valid(raw) {
			t.Fatalf("resource %s returned invalid JSON", resource)
		}
	}

	ledger := p.LedgerStatus(context.Background())
	if !ledger.Validated || !ledger.ReadOnly || ledger.OrderRouting != "MOCK_ONLY" {
		t.Fatalf("unexpected ledger: %+v", ledger)
	}
}

func TestReplayPipelineShowsDecisionLineage(t *testing.T) {
	liquidity, target := 1.5e6, 5000.0
	// Two strategies with half the capital each: only the first signals BTC.
	rsiLine := simulationPipelineStrategy{StrategyID: 1, Name: "Pure_RSI", Capital: 50000, Share: 0.5, Signal: 1, Detail: "RSI(7) 84.3 · #1 by RSI in its universe",
		Sizer: "EqualWeight", SizedWeight: 0.1, CappedWeight: 0.1, ApprovedWeight: 0.1, MaxAsset: 1.5, MaxGross: 1.5, GrossAfterAssetCap: 0.1, GrossScale: 1, Action: "TARGET"}
	otherLine := simulationPipelineStrategy{StrategyID: 2, Name: "Other", Capital: 50000, Share: 0.5, Action: "HOLD"}
	s := simulationSnapshot{BusinessTimestamp: 1, VenueID: "MOCK", Reconciliation: simulationReconciliation{State: "CLEAN"},
		Orders: []simulationOrder{{OrderID: "41", Asset: "BTC", Quantity: 0.1, FilledQuantity: 0.1, Status: "FILLED", ActiveFrom: 20240302}},
		Pipeline: &simulationPipeline{Date: "2024-03-01", DecisionTimestamp: 1, LiquidityLength: 25, LiquidityTopN: 20,
			TotalCapital: 100000, ApprovedGross: 0.05, StrategyCount: 2, ObservedAssets: 200, UniverseSize: 20, ActiveSignals: 1,
			Assets: []simulationPipelineAsset{
				{Asset: "BTC", Close: 50000, Liquidity: &liquidity, LiquidityRank: 1, InUniverse: true,
					Strategies: []simulationPipelineStrategy{rsiLine, otherLine}, Signal: 0.5,
					SizedWeight: 0.05, CappedWeight: 0.05, ApprovedWeight: 0.05, Action: "TARGET", TargetUSD: &target,
					Orders: []simulationPipelineOrder{{OrderID: "41", Side: "BUY", NotionalUSD: 5000, DeltaUSD: 5000}}},
				{Asset: "ETH", Close: 2000, LiquidityRank: 2, InUniverse: true, Action: "HOLD"},
			}}}
	data := NewSimulation(SimulationConfig{}).pipeline(s)
	if data.EvidenceState != "OBSERVED" || len(data.Rows) != 2 || data.ActionablePlans != 1 || len(data.Summary) == 0 {
		t.Fatalf("pipeline = %+v", data)
	}
	btc, eth := data.Rows[0], data.Rows[1]
	if btc.SignalValue == nil || *btc.SignalValue != 0.5 || btc.RawTargetLabel != "5.0%" || btc.DecisionLabel != "ENTER" ||
		btc.TargetLabel != "5,000$" || btc.RequiredDeltaLabel != "+5,000$" || btc.PlannedAction != "BUY 5,000$" || btc.ExchangeState != "FILLED" {
		t.Fatalf("btc row = %+v", btc)
	}
	if len(btc.Strategies) != 2 || btc.Strategies[0].SignalLabel != "+1.00" || btc.Strategies[0].ShareLabel != "50.0%" ||
		btc.Strategies[0].Decision != "ENTER" || btc.Strategies[1].SignalLabel != "0.00" || btc.Strategies[1].Decision != "HOLD" {
		t.Fatalf("btc breakdown = %+v", btc.Strategies)
	}
	if eth.DecisionLabel != "HOLD" || eth.TargetLabel != "—" || eth.ExchangeState != "NONE" {
		t.Fatalf("eth row = %+v", eth)
	}
	if trace := data.Traces[btc.CycleID]; trace.Execution.FillState != "FILLED" || trace.Strategy.Signal != "+0.50 aggregated" {
		t.Fatalf("btc trace = %+v", trace)
	}
}

func TestReplayReconciliationComparesLedgerWithExchange(t *testing.T) {
	s := simulationSnapshot{BusinessTimestamp: 202112239, Reconciliation: simulationReconciliation{State: "CLEAN", LocalSequence: 9, VenueSequence: 9},
		Ledger: simulationLedger{Entries: []simulationLedgerEntry{
			{Kind: "FILL", Asset: "BTCUSDT", PositionDelta: 1},
			{Kind: "FILL", Asset: "BTCUSDT", PositionDelta: 0.5},
			{Kind: "FILL", Asset: "ETHUSDT", PositionDelta: 2},
			{Kind: "FILL", Asset: "ETHUSDT", PositionDelta: -2}, // closed: no row
			{Kind: "FILL", Asset: "SOLUSDT", PositionDelta: 3},
		}},
		Account: simulationAccount{Positions: []simulationPosition{{Asset: "BTCUSDT", Quantity: 1.5}, {Asset: "SOLUSDT", Quantity: 2.9}}},
		Orders:  []simulationOrder{{OrderID: "7", Asset: "XRPUSDT", Side: "SELL", Quantity: 10, RemainingQuantity: 4, Status: "RESTING", ActiveFrom: 202112241}},
	}
	data := NewSimulation(SimulationConfig{}).reconciliation(s)
	rows := data["rows"].([]map[string]any)
	if len(rows) != 3 || data["lastChecked"] != "2021-12-23 close" || data["evidenceFresh"] != true {
		t.Fatalf("reconciliation = %+v", data)
	}
	btc, sol, xrp := rows[0], rows[1], rows[2]
	if btc["asset"] != "BTCUSDT" || btc["status"] != "ALIGNED" || btc["deltaQty"] != 0.0 {
		t.Fatalf("btc = %+v", btc)
	}
	if sol["status"] != "BLOCKED" || sol["deltaQtyLabel"] != "-0.1" {
		t.Fatalf("sol = %+v", sol)
	}
	if xrp["pendingQtyLabel"] != "-4" || xrp["localQty"] != 0.0 || xrp["status"] != "ALIGNED" {
		t.Fatalf("xrp = %+v", xrp)
	}
	if open := data["openOrders"].([]map[string]any); len(open) != 1 || open[0]["age"] != "2021-12-24 open" {
		t.Fatalf("open orders = %+v", open)
	}
}

func TestFormatDollars(t *testing.T) {
	for value, want := range map[float64]string{1234567.4: "1,234,567$", -1000: "-1,000$", 12.5: "12.50$", 0: "0.00$"} {
		if got := formatDollars(value); got != want {
			t.Fatalf("formatDollars(%v) = %s, want %s", value, got, want)
		}
	}
}

func TestReplayDiagnosticsExposeObservationsWithoutFalseCycleProof(t *testing.T) {
	s := simulationSnapshot{Generation: 7, HistoricalDate: "2026-10-07", BusinessTimestamp: 202610079,
		Market:         []simulationMarketBar{{Asset: "BTC", Close: 50000}},
		Account:        simulationAccount{Equity: 100000, CashTotal: 100000, Positions: []simulationPosition{{Asset: "ETH", Quantity: -2, MarkPrice: 2000}}},
		Orders:         []simulationOrder{{OrderID: "old-order", Asset: "BTC", Status: "FILLED"}},
		Fills:          []simulationFill{{OrderID: "old-order", Asset: "BTC", Timestamp: 202609010}},
		Reconciliation: simulationReconciliation{State: "CLEAN"}}
	p := NewSimulation(SimulationConfig{})
	pipeline := p.pipeline(s)
	if len(pipeline.Rows) != 2 || pipeline.EvidenceState != "PARTIAL" || pipeline.Proof != nil || pipeline.Rows[0].Signal != "UNKNOWN" {
		t.Fatalf("false replay lineage: %+v", pipeline)
	}
	if len(pipeline.Traces) != 2 {
		t.Fatal("observed assets lack trace explanations")
	}
	risk := p.risk(s)
	if risk.EvidenceKind != "REPLAY_SNAPSHOT" || !risk.CurrentValuationAvailable || risk.ActiveLimitsAvailable || risk.BreachesAvailable || risk.Assets[0].CurrentWeightLabel != "-4.0%" || risk.Assets[0].ApprovedWeightLabel != "Not exposed" {
		t.Fatalf("false replay risk verdict: %+v", risk)
	}
}

func TestSimulationSlippageIsAdversePositiveAndSummedPerDay(t *testing.T) {
	orders := map[string]simulationOrder{
		"1": {OrderID: "1", Side: "BUY", LimitPrice: 100},
		"2": {OrderID: "2", Side: "SELL", LimitPrice: 100},
	}
	fills := []simulationFill{
		{OrderID: "1", Quantity: 1, Price: 101, Timestamp: 202401021}, // buy 1% above: +100 bps
		{OrderID: "2", Quantity: 3, Price: 100, Timestamp: 202401021}, // sell at expected: 0 bps
		{OrderID: "2", Quantity: 1, Price: 99, Timestamp: 202401031},  // sell 1% below: +100 bps
	}
	summary := simulationSlippage(fills, orders)
	if !summary.available || len(summary.daily) != 2 || summary.worstLabel != "+100.00 bps" || summary.bestLabel != "0.00 bps" {
		t.Fatalf("summary = %+v", summary)
	}
	// Day 1: the sum of the day's fills, +100 + 0 bps.
	if day := summary.daily[0]; day["label"] != "2024-01-02" || math.Abs(day["bps"].(float64)-100) > 1e-9 || day["fills"] != 2 {
		t.Fatalf("day 1 = %+v", day)
	}
}

func TestSimulationServesLastGoodSnapshotOverTornRead(t *testing.T) {
	dir := t.TempDir()
	writeSimulationFixture(t, dir)
	p := NewSimulation(SimulationConfig{Dir: dir})
	if _, err := p.load(); err != nil {
		t.Fatal(err)
	}
	// A half-written file (as seen mid-rewrite) must not surface as an outage.
	if err := os.WriteFile(filepath.Join(dir, "state.json"), []byte(`{"schemaVersion":1,"gener`), 0o600); err != nil {
		t.Fatal(err)
	}
	if s, err := p.load(); err != nil || s.VenueID != "MOCK" {
		t.Fatalf("torn read surfaced: %v", err)
	}
	// Without a recent good snapshot the error stays visible.
	if _, err := NewSimulation(SimulationConfig{Dir: dir}).load(); err == nil {
		t.Fatal("invalid state accepted without a previous good snapshot")
	}
}
