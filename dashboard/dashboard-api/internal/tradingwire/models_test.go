package tradingwire

import "testing"

func TestDecodeTradingStateSnapshot(t *testing.T) {
	raw := []byte(`{
      "schema_version":1,
      "last_bar_close_timestamp":20200416,
      "last_execution_timestamp":20200417,
      "next_order_id":13,
      "account_cash":94919.31164948687,
      "account_positions":{},
      "strategies":[{"strategy_id":1,"signals":{},"desired_weights":{},"virtual_positions":{}}],
      "pending_plans":[],
      "orders":[{"order_id":12,"strategy_id":1,"created_at":20200414,"active_from":20200415,"coin":"LINK","side":1,"quantity":3010.51210617,"status":4,"filled_quantity":3010.51210617,"updated_at":20200415,"cancel_requested":false,"exchange_order_id":"sim-12","last_message":"filled"}],
      "processed_fill_ids":[1,2,3,4,5,6,7,8,9,10,11,12]
    }`)
	value, err := DecodeTradingStateSnapshot(raw)
	if err != nil {
		t.Fatalf("DecodeTradingStateSnapshot() error = %v", err)
	}
	if value.LastExecutionTimestamp != 20200417 || value.AccountCash != 94919.31164948687 || len(value.Orders) != 1 {
		t.Fatalf("unexpected snapshot: %+v", value)
	}
}

func TestDecodeStrategyRiskPlannerWirePayloads(t *testing.T) {
	metadata := `"metadata":{"schema_version":1,"message_id":"m1","correlation_id":"c1","produced_at":20200416}`
	intent := []byte(`{` + metadata + `,"timestamp":20200416,"strategies":[{"strategy_id":1,"strategy_name":"Pure_RSI","signals":{"BTC":1}}]}`)
	if _, err := DecodeStrategyIntentBatch(intent); err != nil {
		t.Fatalf("DecodeStrategyIntentBatch() error = %v", err)
	}

	decision := []byte(`{` + metadata + `,"decision_timestamp":20200416,"strategies":[{"strategy_id":1,"decision_timestamp":20200416,"reference_capital":100000,"target_notional_usd":{"BTC":10000},"decisions":[{"coin":"BTC","action":2,"target_weight":0.1}]}]}`)
	if _, err := DecodeDecisionBatch(decision); err != nil {
		t.Fatalf("DecodeDecisionBatch() error = %v", err)
	}

	request := []byte(`{` + metadata + `,"decision_timestamp":20200416,"decisions":` + string(decision) + `,"reference_closes":{` + metadata + `,"date":20200416,"closes":{"BTC":7000}},"state":{"state_revision":123,"strategy_ids":[1],"strategy_positions":[{"strategy_id":1,"positions":{}}],"orders":[],"next_order_id":1}}`)
	if _, err := DecodeNotionalPlanningRequest(request); err != nil {
		t.Fatalf("DecodeNotionalPlanningRequest() error = %v", err)
	}

	plan := []byte(`{` + metadata + `,"decision_timestamp":20200416,"state_revision":123,"decisions":` + string(decision) + `,"reference_closes":{` + metadata + `,"date":20200416,"closes":{"BTC":7000}},"next_order_id":2,"cancel_order_ids":[],"submit_orders":[{"economic_order_id":"planned-notional-order:20200416:123:1","order_id":1,"strategy_id":1,"created_at":20200416,"decision_timestamp":20200416,"state_revision":123,"coin":"BTC","side":0,"reference_close":7000,"target_notional_usd":10000,"current_notional_usd":0,"pending_notional_usd":0,"delta_notional_usd":10000,"notional_usd":10000}],"global_target_notional_usd":{"BTC":10000}}`)
	if _, err := DecodeNotionalOrderPlan(plan); err != nil {
		t.Fatalf("DecodeNotionalOrderPlan() error = %v", err)
	}
}

func TestDecodeExchangeSnapshotEvent(t *testing.T) {
	raw := []byte(`{"metadata":{"schema_version":1,"message_id":"snap-1","correlation_id":"req-1","produced_at":20260924},"snapshot":{"timestamp":20260924,"cash":99900.5,"positions":{"BTC":0.25},"open_orders":[{"local_order_id":7,"exchange_order_id":"ex-7","coin":"ETH","side":0,"quantity":2,"filled_quantity":0.5}]}}`)
	value, err := DecodeExchangeSnapshotEvent(raw)
	if err != nil {
		t.Fatal(err)
	}
	if value.Snapshot.Timestamp != 20260924 || value.Snapshot.Positions["BTC"] != 0.25 || len(value.Snapshot.OpenOrders) != 1 {
		t.Fatalf("unexpected exchange snapshot: %+v", value)
	}
}

func TestDecodeAccountSnapshotAcceptsObjectStrategyPositions(t *testing.T) {
	raw := []byte(`{"metadata":{"schema_version":1,"message_id":"acct-object","correlation_id":"c","produced_at":20260919},"timestamp":20260919,"cash":100000,"positions":{"BTC":0.1},"strategy_positions":{"1":{"BTC":0.1}}}`)
	value, err := DecodeAccountSnapshot(raw)
	if err != nil {
		t.Fatalf("DecodeAccountSnapshot(object) error = %v", err)
	}
	if value.StrategyPositions["1"]["BTC"] != 0.1 {
		t.Fatalf("unexpected object strategy positions: %+v", value.StrategyPositions)
	}
}

func TestDecodeAccountSnapshotAcceptsArrayStrategyPositions(t *testing.T) {
	raw := []byte(`{"metadata":{"schema_version":1,"message_id":"acct-array","correlation_id":"c","produced_at":20260919},"timestamp":20260919,"cash":100000,"positions":{"BTC":0.1},"strategy_positions":[{"strategy_id":1,"positions":{"BTC":0.1}}]}`)
	value, err := DecodeAccountSnapshot(raw)
	if err != nil {
		t.Fatalf("DecodeAccountSnapshot(array) error = %v", err)
	}
	if value.StrategyPositions["1"]["BTC"] != 0.1 {
		t.Fatalf("unexpected array strategy positions: %+v", value.StrategyPositions)
	}
}

func TestDecodeAccountSnapshotAcceptsTupleStrategyPositions(t *testing.T) {
	raw := []byte(`{"metadata":{"schema_version":1,"message_id":"acct-tuple","correlation_id":"c","produced_at":20260919},"timestamp":20260919,"cash":100000,"positions":{"BTCUSDT":0.1},"strategy_positions":[[1,{"BTCUSDT":0.1}]]}`)
	value, err := DecodeAccountSnapshot(raw)
	if err != nil {
		t.Fatalf("DecodeAccountSnapshot(tuple) error = %v", err)
	}
	if value.StrategyPositions["1"]["BTCUSDT"] != 0.1 {
		t.Fatalf("unexpected tuple strategy positions: %+v", value.StrategyPositions)
	}
}
