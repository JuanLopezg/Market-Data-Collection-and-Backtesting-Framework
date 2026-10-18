package provider

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	pgstore "control-dashboard-api/internal/integration/postgres"
	"control-dashboard-api/internal/tradingwire"
)

// The NEAR cycle of 2026-10-17: a held long inside the 25% relative band.
func TestTraceExplainsHoldInsideTheRelativeBand(t *testing.T) {
	meta := tradingwire.ContractMetadata{SchemaVersion: 1, MessageID: "m", CorrelationID: "market-data-updated:20261017"}
	signals := func(day uint64) tradingwire.StrategyIntentBatch {
		return tradingwire.StrategyIntentBatch{Metadata: meta, Timestamp: day, Strategies: []tradingwire.StrategySignalIntent{
			{StrategyID: 1, StrategyName: "Pure_RSI", Signals: map[string]float64{"NEARUSDT": 1}}}}
	}
	previous := signals(20261016)
	weight, held := 0.10, 4.0
	checkpoint := pgstore.PipelineCheckpoint{
		Timestamp: 20261017, Signals: signals(20261017), PreviousSignals: &previous,
		PortfolioConfig: `{"strategies":[{"id":1,"sizer":{"type":"equal_weight","weight_per_full_signal":0.10},"rebalance":{"type":"relative_threshold","threshold":0.25}}]}`,
		Account:         tradingwire.AccountSnapshot{Metadata: meta, Timestamp: 20261017, Positions: map[string]float64{"NEARUSDT": 4}},
		Decision:        tradingwire.DecisionBatch{Metadata: meta, DecisionTimestamp: 20261017},
		RiskDiagnostics: &pgstore.RiskDiagnostics{ConfigurationFingerprint: "abc123", Strategies: []pgstore.RiskStrategyEvaluation{{
			StrategyID: 1, Sizer: "EqualWeight", ReferenceCapital: 226.57, MaxAssetWeight: 1.5, MaxGrossLeverage: 1.5,
			GrossAfterAssetCap: 0.3, GrossScale: 1, VolatilityState: "NOT_APPLICABLE",
			Assets: map[string]pgstore.RiskAssetEvaluation{"NEARUSDT": {SizedWeight: &weight, AssetCappedWeight: &weight,
				ApprovedWeight: &weight, CurrentQuantity: &held, Action: "HOLD"}}}}},
	}
	request := tradingwire.NotionalOrderPlanningRequest{Metadata: meta, DecisionTimestamp: 20261017,
		ReferenceCloses: tradingwire.DailyCloseSnapshot{Date: 20261017, Closes: map[string]float64{"NEARUSDT": 4.86}},
		State: tradingwire.ExecutionPlanningState{StateRevision: 3, StrategyPositions: []tradingwire.PlanningStrategyPositions{
			{StrategyID: 1, Positions: map[string]float64{"NEARUSDT": 4}}}}}
	checkpoint.PlanningRequest = &request
	checkpoint.Plan = &tradingwire.NotionalOrderPlanBatch{Metadata: meta, DecisionTimestamp: 20261017, StateRevision: 3}

	data := buildRealPipeline(checkpoint, pgstore.RuntimeState{}, realReconciliationData{}, nil, "LIVE")
	cfg := applyStrategyConfigFile(RealConfig{RSILength: 8, RSIEntry: 50, RSIExit: 40, StrategyUniverseN: 20, QuoteVolume: true})
	enrichPipeline(&data, checkpoint, pipelineTraceInputs{cfg: cfg, exchange: "Kraken Futures (LIVE)",
		market: map[string]marketAssetView{"NEARUSDT": {rank: 7, candidates: 50, universeN: 20, smaVolume: 1.2e8, rsi: 63.2, latestDate: 20261017}}})

	row := data.Rows[0]
	if row.RankLabel != "#7" || row.TargetLabel != "22.66$ (hold)" || row.RSILabel != "63.2" {
		t.Fatalf("row = %+v", row)
	}
	trace := data.Traces[row.CycleID]
	checks := map[string]string{
		"rank":       trace.Market.LiquidityRank,
		"rsi":        trace.Strategy.RSI,
		"rule":       trace.Strategy.Rule,
		"transition": trace.Strategy.Transition,
		"sizer":      trace.Portfolio.Sizer,
		"raw":        trace.Portfolio.RawTarget,
		"rebalance":  trace.Portfolio.Rebalance,
		"approved":   trace.Portfolio.ApprovedTarget,
		"exchange":   trace.Audit.Exchange,
		"config":     trace.Audit.ConfigVersion,
	}
	expected := map[string]string{
		"rank":       "#7 of 50 by SMA Quote Volume 25 (USDT)",
		"rsi":        "RSI(8) = 63.2",
		"rule":       "enter when RSI(8) > 50, exit when RSI(8) < 40",
		"transition": "LONG (2026-10-16) → LONG (2026-10-17): held",
		"sizer":      "Equal weight: 10.0%",
		"raw":        "signal +1.00 × 10.0% = 10.0% of 226.57$ = 22.66$",
		"rebalance":  "Relative band 25.0%: current 8.6%",
		"approved":   "HOLD at 10.0% approved (22.66$): keeps +4, no order",
		"exchange":   "Kraken Futures (LIVE)",
		"config":     "risk config abc123",
	}
	for key, want := range expected {
		if !strings.Contains(checks[key], want) {
			t.Errorf("%s = %q, want it to contain %q", key, checks[key], want)
		}
	}
	if !strings.HasSuffix(trace.Portfolio.Rebalance, "→ HOLD") || trace.Portfolio.VolTarget != "" {
		t.Errorf("rebalance %q, vol target %q", trace.Portfolio.Rebalance, trace.Portfolio.VolTarget)
	}
}

// The dashboard reads the live strategy's parameters from its own config file.
func TestStrategyConfigFileOverridesEnvironmentDefaults(t *testing.T) {
	// Same content as config/strategies/pure_rsi_8_50_40_volume_rank.json.
	path := filepath.Join(t.TempDir(), "strategy.json")
	file := `{"strategies":[{"id":1,"type":"PureRSI","max_active_signals":10,"max_ranking_position":1000000,
		"universe":{"type":"top_n_liquidity","count":20,"descending":true,"indicator":{"kind":"SMA","source":"QuoteVolume","length":25,"offset":0}},
		"ranker":{"type":"indicator","descending":true,"indicator":{"kind":"SMA","source":"QuoteVolume","length":25,"offset":0}},
		"parameters":{"rsi_length":8,"rsi_entry":50.0,"rsi_exit":40.0}}]}`
	if err := os.WriteFile(path, []byte(file), 0o600); err != nil {
		t.Fatal(err)
	}
	cfg := applyStrategyConfigFile(RealConfig{RSIEntry: 80, RSIExit: 70, StrategyConfigFile: path})
	if cfg.rsiLength() != 8 || cfg.RSIEntry != 50 || cfg.RSIExit != 40 || cfg.maxActiveSignals() != 10 ||
		cfg.StrategyUniverseN != 20 || cfg.liquidityLength() != 25 || !cfg.QuoteVolume {
		t.Fatalf("cfg = %+v", cfg)
	}
	missing := applyStrategyConfigFile(RealConfig{RSIEntry: 80, RSIExit: 70, StrategyConfigFile: "missing.json"})
	if missing.RSIEntry != 80 || !strings.Contains(missing.StrategyConfigNote, "unreadable") {
		t.Fatalf("missing file = %+v", missing)
	}
}
