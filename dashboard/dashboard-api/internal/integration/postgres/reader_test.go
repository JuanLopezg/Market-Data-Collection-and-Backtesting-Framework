package postgres

import (
	"context"
	"encoding/json"
	"strings"
	"testing"
	"time"

	"control-dashboard-api/internal/tradingwire"
)

type fakeRunner struct {
	responses [][]byte
	queries   []string
}

func (f *fakeRunner) QueryJSON(_ context.Context, _ string, query string) ([]byte, error) {
	f.queries = append(f.queries, query)
	if len(f.responses) == 0 {
		return nil, nil
	}
	result := f.responses[0]
	f.responses = f.responses[1:]
	return result, nil
}

func TestReaderSnapshotWithRuntime(t *testing.T) {
	runner := &fakeRunner{responses: [][]byte{
		[]byte(`{"activeConnections":4,"maxConnections":100,"databaseSizeBytes":1048576,"tables":{"runtimeState":true,"tradingFills":true,"strategyCheckpoint":false,"riskCheckpoint":false,"plannerCheckpoint":false}}`),
		[]byte(`{"schemaVersion":1,"updatedAt":"2026-09-24T16:00:00.000Z","lastBarCloseTimestamp":20260923,"lastExecutionTimestamp":20260924,"accountCash":101234.5,"positionCount":2,"strategyCount":1,"pendingPlanCount":0,"trackedOrderCount":3,"processedFillCount":7}`),
		[]byte(`{"fillRows":7,"latestFillTimestamp":20260924}`),
	}}
	reader := NewReader(Config{DSN: "host=postgres dbname=algotrading user=algotrading password=x", Timeout: time.Second, Runner: runner})
	snapshot, err := reader.Snapshot(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	if !snapshot.Runtime.Present || snapshot.Runtime.AccountCash != 101234.5 || snapshot.Runtime.FillRows != 7 {
		t.Fatalf("unexpected runtime snapshot: %+v", snapshot.Runtime)
	}
	if len(runner.queries) != 3 {
		t.Fatalf("expected 3 bounded queries, got %d", len(runner.queries))
	}
}

func TestPostgresEnvDoesNotPutPasswordInCommandArgs(t *testing.T) {
	env, err := postgresEnv("host=postgres port=5432 dbname=algotrading user=juan password=secret")
	if err != nil {
		t.Fatal(err)
	}
	joined := strings.Join(env, "\n")
	if !strings.Contains(joined, "PGPASSWORD=secret") || !strings.Contains(joined, "PGUSER=juan") {
		t.Fatalf("unexpected env: %v", env)
	}
}

func TestParseURLDSN(t *testing.T) {
	values, err := parseDSN("postgres://user:pass@postgres:5432/algotrading?sslmode=disable")
	if err != nil {
		t.Fatal(err)
	}
	if values["host"] != "postgres" || values["user"] != "user" || values["dbname"] != "algotrading" || values["sslmode"] != "disable" {
		t.Fatalf("unexpected values: %#v", values)
	}
}

func TestRuntimeSummarySQLUsesSupportedJSONFunctions(t *testing.T) {
	if strings.Contains(runtimeSummarySQL, "jsonb_object_length") {
		t.Fatal("runtime summary must not use unsupported jsonb_object_length")
	}
	if !strings.Contains(runtimeSummarySQL, "jsonb_object_keys") || !strings.Contains(runtimeSummarySQL, "jsonb_typeof") {
		t.Fatal("runtime summary must count object keys safely")
	}
}

func TestReaderRuntimeStateDecodesVerifiedSnapshot(t *testing.T) {
	runner := &fakeRunner{responses: [][]byte{
		[]byte(`{"updatedAt":"2026-09-24T17:00:00.000Z","snapshot":{"schema_version":1,"last_bar_close_timestamp":20260923,"last_execution_timestamp":0,"next_order_id":4,"account_cash":95000.5,"account_positions":{"BTC":0.25,"ETH":-2},"strategies":[{"strategy_id":1,"signals":{},"desired_weights":{},"virtual_positions":{"BTC":0.25,"ETH":-2}}],"pending_plans":[],"orders":[],"processed_fill_ids":[]}}`),
	}}
	reader := NewReader(Config{DSN: "host=postgres dbname=algotrading user=algotrading password=x", Timeout: time.Second, Runner: runner})
	state, err := reader.RuntimeState(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	if state.UpdatedAt != "2026-09-24T17:00:00.000Z" || state.Snapshot.AccountCash != 95000.5 || state.Snapshot.AccountPositions["BTC"] != 0.25 {
		t.Fatalf("unexpected runtime state: %+v", state)
	}
	if len(runner.queries) != 1 || !strings.Contains(runner.queries[0], "snapshot") {
		t.Fatalf("expected one bounded runtime-state query, got %v", runner.queries)
	}
}

func TestReaderFillsForOrdersUsesBoundedNumericFilter(t *testing.T) {
	runner := &fakeRunner{responses: [][]byte{
		[]byte(`{"totalRows":3,"totalCommission":1.25,"rows":[{"fill_id":10,"order_id":7,"strategy_id":1,"timestamp":20260924,"coin":"BTC","side":0,"quantity":0.1,"price":50000,"commission":0.5}]}`),
	}}
	reader := NewReader(Config{DSN: "host=postgres dbname=algotrading user=algotrading password=x", Timeout: time.Second, Runner: runner})
	window, err := reader.FillsForOrders(context.Background(), []uint64{7, 7, 9, 0}, 25)
	if err != nil {
		t.Fatal(err)
	}
	if window.TotalRows != 3 || window.TotalCommission != 1.25 || len(window.Rows) != 1 || window.Rows[0].OrderID != 7 {
		t.Fatalf("unexpected fill window: %+v", window)
	}
	if len(runner.queries) != 1 || !strings.Contains(runner.queries[0], "order_id IN (7,9)") || !strings.Contains(runner.queries[0], "LIMIT 25") {
		t.Fatalf("expected bounded numeric fill query, got %v", runner.queries)
	}
}

func TestLatestPipelineCheckpointKeepsOneDecisionTimestamp(t *testing.T) {
	encode := func(v any) string {
		raw, err := json.Marshal(v)
		if err != nil {
			t.Fatal(err)
		}
		return string(raw)
	}
	meta := tradingwire.ContractMetadata{SchemaVersion: 1, MessageID: "m", CorrelationID: "corr", ProducedAt: 20260919}
	signals := tradingwire.StrategyIntentBatch{Metadata: meta, Timestamp: 20260919, Strategies: []tradingwire.StrategySignalIntent{{StrategyID: 1, StrategyName: "PureRSI", Signals: map[string]float64{"BTC": 1}}}}
	account := tradingwire.AccountSnapshot{Metadata: meta, Timestamp: 20260919, Cash: 100000, Positions: map[string]float64{}, StrategyPositions: map[string]map[string]float64{}}
	decision := tradingwire.DecisionBatch{Metadata: meta, DecisionTimestamp: 20260919, Strategies: []tradingwire.StrategyDecisionIntent{{StrategyID: 1, DecisionTimestamp: 20260919, ReferenceCapital: 100000, TargetNotionalUSD: map[string]float64{"BTC": 10000}, Decisions: []tradingwire.RebalanceDecision{{Coin: "BTC", Action: 2, TargetWeight: .1}}}}}
	update := tradingwire.MarketDataUpdated{Metadata: meta, CompletedThrough: 20260919, Source: "binance", Timeframe: "1d", RankedSymbols: 20, ActiveTopN: 20, TrackedSymbols: 20, MaintainedSymbols: 20}
	envelope, _ := json.Marshal(map[string]any{"timestamp": 20260919, "strategyUpdatePayload": encode(update), "signalsPayload": encode(signals), "accountPayload": encode(account), "decisionPayload": encode(decision), "planningRequestPayload": nil, "planPayload": nil})
	runner := &fakeRunner{responses: [][]byte{envelope}}
	reader := NewReader(Config{DSN: "host=postgres dbname=algotrading user=algotrading password=x", Timeout: time.Second, Runner: runner})
	checkpoint, err := reader.LatestPipelineCheckpoint(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	if checkpoint.Timestamp != 20260919 || checkpoint.Signals.Timestamp != 20260919 || checkpoint.Decision.DecisionTimestamp != 20260919 || checkpoint.StrategyUpdate == nil || checkpoint.StrategyUpdate.ActiveTopN != 20 {
		t.Fatalf("unexpected pipeline checkpoint: %+v", checkpoint)
	}
	if checkpoint.Plan != nil || checkpoint.PlanningRequest != nil {
		t.Fatalf("planner should remain optional: %+v", checkpoint)
	}
	if len(runner.queries) != 1 || !strings.Contains(runner.queries[0], "portfolio-risk-live-sqlite-v1") || !strings.Contains(runner.queries[0], "timestamp = risk.timestamp") {
		t.Fatalf("pipeline query must be cycle aligned: %v", runner.queries)
	}
}

func TestLatestStrategyCheckpointKeepsUpdateAndIntentAligned(t *testing.T) {
	update := tradingwire.MarketDataUpdated{Metadata: tradingwire.ContractMetadata{SchemaVersion: 1, MessageID: "market-data-updated:20260919"}, CompletedThrough: 20260919, Source: "binance", Timeframe: "1d", ActiveTopN: 50}
	intents := tradingwire.StrategyIntentBatch{Metadata: tradingwire.ContractMetadata{SchemaVersion: 1, MessageID: "strategy-intents:20260919"}, Timestamp: 20260919, Strategies: []tradingwire.StrategySignalIntent{{StrategyID: 1, StrategyName: "Pure_RSI", Signals: map[string]float64{"BTCUSDT": 1}}}}
	updateRaw, _ := json.Marshal(update)
	intentRaw, _ := json.Marshal(intents)
	envelope, _ := json.Marshal(map[string]any{"timestamp": 20260919, "updatePayload": string(updateRaw), "intentPayload": string(intentRaw)})
	runner := &fakeRunner{responses: [][]byte{envelope}}
	reader := NewReader(Config{DSN: "host=postgres dbname=algotrading user=algotrading password=x", Timeout: time.Second, Runner: runner})
	checkpoint, err := reader.LatestStrategyCheckpoint(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	if checkpoint.Timestamp != 20260919 || checkpoint.Update.ActiveTopN != 50 || checkpoint.Intents.Timestamp != 20260919 {
		t.Fatalf("unexpected strategy checkpoint: %+v", checkpoint)
	}
	if len(runner.queries) != 1 || !strings.Contains(runner.queries[0], "strategy_market_update_checkpoint") {
		t.Fatalf("unexpected strategy query: %v", runner.queries)
	}
}

func TestReaderLedgerFillsReturnsFullAggregatesAndBoundedRows(t *testing.T) {
	runner := &fakeRunner{responses: [][]byte{
		[]byte(`{"totalRows":12,"distinctFillIds":12,"invalidRows":0,"totalCommission":3.5,"grossBuyNotional":1000,"grossSellNotional":1200,"latestFillId":99,"latestFillTimestamp":20260926,"rows":[{"fill_id":99,"order_id":77,"strategy_id":1,"timestamp":20260926,"coin":"BTCUSDT","side":1,"quantity":0.1,"price":12000,"commission":0.5}]}`),
	}}
	reader := NewReader(Config{DSN: "host=postgres dbname=algotrading user=algotrading password=x", Timeout: time.Second, Runner: runner})
	window, err := reader.LedgerFills(context.Background(), 25)
	if err != nil {
		t.Fatal(err)
	}
	if window.TotalRows != 12 || window.DistinctFillIDs != 12 || window.InvalidRows != 0 || window.LatestFillID != 99 || len(window.Rows) != 1 {
		t.Fatalf("unexpected ledger window: %+v", window)
	}
	if len(runner.queries) != 1 || !strings.Contains(runner.queries[0], "count(DISTINCT fill_id)") || !strings.Contains(runner.queries[0], "LIMIT 25") {
		t.Fatalf("ledger query must be aggregate + bounded detail: %v", runner.queries)
	}
}
