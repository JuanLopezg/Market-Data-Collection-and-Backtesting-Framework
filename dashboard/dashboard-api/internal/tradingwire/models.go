package tradingwire

import (
	"encoding/json"
	"fmt"
	"math"
)

// These DTOs mirror the verified C++ JSON/persistence contracts used by the
// trading runtime. They are read-only dashboard representations: the dashboard
// must never use them to generate or submit trading decisions.

type ContractMetadata struct {
	SchemaVersion uint32 `json:"schema_version"`
	MessageID     string `json:"message_id"`
	CorrelationID string `json:"correlation_id"`
	ProducedAt    uint64 `json:"produced_at"`
}

type RebalanceDecision struct {
	Coin         string  `json:"coin"`
	Action       int     `json:"action"`
	TargetWeight float64 `json:"target_weight"`
}

type PendingPlan struct {
	StrategyID       uint64              `json:"strategy_id"`
	Timestamp        uint64              `json:"timestamp"`
	ReferenceCapital float64             `json:"reference_capital"`
	Decisions        []RebalanceDecision `json:"decisions"`
}

type StrategyStateSnapshot struct {
	StrategyID       uint64             `json:"strategy_id"`
	Signals          map[string]float64 `json:"signals"`
	DesiredWeights   map[string]float64 `json:"desired_weights"`
	VirtualPositions map[string]float64 `json:"virtual_positions"`
}

type PersistedTrackedOrder struct {
	OrderID         uint64  `json:"order_id"`
	StrategyID      uint64  `json:"strategy_id"`
	CreatedAt       uint64  `json:"created_at"`
	ActiveFrom      uint64  `json:"active_from"`
	Coin            string  `json:"coin"`
	Side            int     `json:"side"`
	Quantity        float64 `json:"quantity"`
	Status          int     `json:"status"`
	FilledQuantity  float64 `json:"filled_quantity"`
	UpdatedAt       uint64  `json:"updated_at"`
	CancelRequested bool    `json:"cancel_requested"`
	ExchangeOrderID string  `json:"exchange_order_id"`
	LastMessage     string  `json:"last_message"`
}

type TradingStateSnapshot struct {
	SchemaVersion          uint32                  `json:"schema_version"`
	LastBarCloseTimestamp  uint64                  `json:"last_bar_close_timestamp"`
	LastExecutionTimestamp uint64                  `json:"last_execution_timestamp"`
	NextOrderID            uint64                  `json:"next_order_id"`
	AccountCash            float64                 `json:"account_cash"`
	AccountPositions       map[string]float64      `json:"account_positions"`
	Strategies             []StrategyStateSnapshot `json:"strategies"`
	PendingPlans           []PendingPlan           `json:"pending_plans"`
	Orders                 []PersistedTrackedOrder `json:"orders"`
	ProcessedFillIDs       []uint64                `json:"processed_fill_ids"`
}

type Fill struct {
	FillID     uint64  `json:"fill_id"`
	OrderID    uint64  `json:"order_id"`
	StrategyID uint64  `json:"strategy_id"`
	Timestamp  uint64  `json:"timestamp"`
	Coin       string  `json:"coin"`
	Side       int     `json:"side"`
	Quantity   float64 `json:"quantity"`
	Price      float64 `json:"price"`
	Commission float64 `json:"commission"`
}

type MarketDataUpdated struct {
	Metadata          ContractMetadata `json:"metadata"`
	CompletedThrough  uint64           `json:"completed_through"`
	Source            string           `json:"source"`
	Timeframe         string           `json:"timeframe"`
	RankedSymbols     uint64           `json:"ranked_symbols"`
	ActiveTopN        uint64           `json:"active_top_n"`
	TrackedSymbols    uint64           `json:"tracked_symbols"`
	MaintainedSymbols uint64           `json:"maintained_symbols"`
	RequestedSymbols  uint64           `json:"requested_symbols"`
	DownloadedRows    uint64           `json:"downloaded_rows"`
}

type StrategySignalIntent struct {
	StrategyID   uint64             `json:"strategy_id"`
	StrategyName string             `json:"strategy_name"`
	Signals      map[string]float64 `json:"signals"`
}

type StrategyIntentBatch struct {
	Metadata   ContractMetadata       `json:"metadata"`
	Timestamp  uint64                 `json:"timestamp"`
	Strategies []StrategySignalIntent `json:"strategies"`
}

type StrategyDecisionIntent struct {
	StrategyID        uint64              `json:"strategy_id"`
	DecisionTimestamp uint64              `json:"decision_timestamp"`
	ReferenceCapital  float64             `json:"reference_capital"`
	TargetNotionalUSD map[string]float64  `json:"target_notional_usd"`
	Decisions         []RebalanceDecision `json:"decisions"`
}

type DecisionBatch struct {
	Metadata          ContractMetadata         `json:"metadata"`
	DecisionTimestamp uint64                   `json:"decision_timestamp"`
	Strategies        []StrategyDecisionIntent `json:"strategies"`
}

type DailyCloseSnapshot struct {
	Metadata ContractMetadata   `json:"metadata"`
	Date     uint64             `json:"date"`
	Closes   map[string]float64 `json:"closes"`
}

type ExecutionOrder struct {
	OrderID    uint64  `json:"order_id"`
	StrategyID uint64  `json:"strategy_id"`
	CreatedAt  uint64  `json:"created_at"`
	ActiveFrom uint64  `json:"active_from"`
	Coin       string  `json:"coin"`
	Side       int     `json:"side"`
	Quantity   float64 `json:"quantity"`
}

type WireTrackedOrder struct {
	Request         ExecutionOrder `json:"request"`
	Status          int            `json:"status"`
	FilledQuantity  float64        `json:"filled_quantity"`
	UpdatedAt       uint64         `json:"updated_at"`
	CancelRequested bool           `json:"cancel_requested"`
	ExchangeOrderID string         `json:"exchange_order_id"`
	LastMessage     string         `json:"last_message"`
}

type PlanningStrategyPositions struct {
	StrategyID uint64             `json:"strategy_id"`
	Positions  map[string]float64 `json:"positions"`
}

type ExecutionPlanningState struct {
	StateRevision     uint64                      `json:"state_revision"`
	StrategyIDs       []uint64                    `json:"strategy_ids"`
	StrategyPositions []PlanningStrategyPositions `json:"strategy_positions"`
	Orders            []WireTrackedOrder          `json:"orders"`
	NextOrderID       uint64                      `json:"next_order_id"`
}

type NotionalOrderPlanningRequest struct {
	Metadata          ContractMetadata       `json:"metadata"`
	DecisionTimestamp uint64                 `json:"decision_timestamp"`
	Decisions         DecisionBatch          `json:"decisions"`
	ReferenceCloses   DailyCloseSnapshot     `json:"reference_closes"`
	State             ExecutionPlanningState `json:"state"`
}

type PlannedNotionalOrder struct {
	EconomicOrderID    string  `json:"economic_order_id"`
	OrderID            uint64  `json:"order_id"`
	StrategyID         uint64  `json:"strategy_id"`
	CreatedAt          uint64  `json:"created_at"`
	DecisionTimestamp  uint64  `json:"decision_timestamp"`
	StateRevision      uint64  `json:"state_revision"`
	Coin               string  `json:"coin"`
	Side               int     `json:"side"`
	ReferenceClose     float64 `json:"reference_close"`
	TargetNotionalUSD  float64 `json:"target_notional_usd"`
	CurrentNotionalUSD float64 `json:"current_notional_usd"`
	PendingNotionalUSD float64 `json:"pending_notional_usd"`
	DeltaNotionalUSD   float64 `json:"delta_notional_usd"`
	NotionalUSD        float64 `json:"notional_usd"`
}

type NotionalOrderPlanBatch struct {
	Metadata                ContractMetadata       `json:"metadata"`
	DecisionTimestamp       uint64                 `json:"decision_timestamp"`
	StateRevision           uint64                 `json:"state_revision"`
	Decisions               DecisionBatch          `json:"decisions"`
	ReferenceCloses         DailyCloseSnapshot     `json:"reference_closes"`
	NextOrderID             uint64                 `json:"next_order_id"`
	CancelOrderIDs          []uint64               `json:"cancel_order_ids"`
	SubmitOrders            []PlannedNotionalOrder `json:"submit_orders"`
	GlobalTargetNotionalUSD map[string]float64     `json:"global_target_notional_usd"`
}

type StrategyPositionMap map[string]map[string]float64

// UnmarshalJSON accepts all AccountSnapshot encodings observed in the LIVE
// runtime/checkpoint history. Depending on the producer/persistence boundary,
// strategy_positions can be encoded as:
//
//	{"1":{"BTCUSDT":0.1}}
//	[{"strategy_id":1,"positions":{"BTCUSDT":0.1}}]
//	[[1,{"BTCUSDT":0.1}]]
//
// The dashboard is read-only, so all representations are normalized to a map
// without changing trading state or relaxing the trading-side contracts.
func (m *StrategyPositionMap) UnmarshalJSON(raw []byte) error {
	if string(raw) == "null" {
		*m = StrategyPositionMap{}
		return nil
	}

	var object map[string]map[string]float64
	if err := json.Unmarshal(raw, &object); err == nil {
		*m = StrategyPositionMap(object)
		return nil
	}

	var elements []json.RawMessage
	if err := json.Unmarshal(raw, &elements); err != nil {
		return fmt.Errorf("decode strategy_positions as object or array: %w", err)
	}

	normalized := make(StrategyPositionMap, len(elements))
	add := func(strategyID uint64, positions map[string]float64) error {
		if strategyID == 0 {
			return fmt.Errorf("strategy_positions contains zero strategy_id")
		}
		key := fmt.Sprintf("%d", strategyID)
		if _, exists := normalized[key]; exists {
			return fmt.Errorf("strategy_positions contains duplicate strategy_id %d", strategyID)
		}
		if positions == nil {
			positions = map[string]float64{}
		}
		normalized[key] = positions
		return nil
	}

	for i, element := range elements {
		var item PlanningStrategyPositions
		if err := json.Unmarshal(element, &item); err == nil && item.StrategyID != 0 {
			if err := add(item.StrategyID, item.Positions); err != nil {
				return err
			}
			continue
		}

		var tuple []json.RawMessage
		if err := json.Unmarshal(element, &tuple); err != nil || len(tuple) != 2 {
			return fmt.Errorf("strategy_positions[%d] is neither planning object nor [strategy_id, positions] tuple", i)
		}
		var strategyID uint64
		if err := json.Unmarshal(tuple[0], &strategyID); err != nil {
			return fmt.Errorf("decode strategy_positions[%d] strategy_id: %w", i, err)
		}
		var positions map[string]float64
		if string(tuple[1]) != "null" {
			if err := json.Unmarshal(tuple[1], &positions); err != nil {
				return fmt.Errorf("decode strategy_positions[%d] positions: %w", i, err)
			}
		}
		if err := add(strategyID, positions); err != nil {
			return err
		}
	}

	*m = normalized
	return nil
}

type AccountSnapshot struct {
	Metadata          ContractMetadata    `json:"metadata"`
	Timestamp         uint64              `json:"timestamp"`
	Cash              float64             `json:"cash"`
	Positions         map[string]float64  `json:"positions"`
	StrategyPositions StrategyPositionMap `json:"strategy_positions"`
}

func DecodeTradingStateSnapshot(raw []byte) (TradingStateSnapshot, error) {
	var value TradingStateSnapshot
	if err := json.Unmarshal(raw, &value); err != nil {
		return value, fmt.Errorf("decode trading runtime snapshot: %w", err)
	}
	if value.SchemaVersion == 0 || !finite(value.AccountCash) {
		return value, fmt.Errorf("invalid trading runtime snapshot")
	}
	for coin, quantity := range value.AccountPositions {
		if coin == "" || !finite(quantity) {
			return value, fmt.Errorf("invalid account position in trading runtime snapshot")
		}
	}
	return value, nil
}

func DecodeMarketDataUpdated(raw []byte) (MarketDataUpdated, error) {
	var value MarketDataUpdated
	if err := json.Unmarshal(raw, &value); err != nil {
		return value, fmt.Errorf("decode market-data updated event: %w", err)
	}
	if value.CompletedThrough == 0 || value.Metadata.SchemaVersion == 0 || value.Metadata.MessageID == "" {
		return value, fmt.Errorf("invalid market-data updated event")
	}
	return value, nil
}

func DecodeAccountSnapshot(raw []byte) (AccountSnapshot, error) {
	var value AccountSnapshot
	if err := json.Unmarshal(raw, &value); err != nil {
		return value, fmt.Errorf("decode account snapshot: %w", err)
	}
	if value.Timestamp == 0 || value.Metadata.SchemaVersion == 0 || value.Metadata.MessageID == "" || !finite(value.Cash) {
		return value, fmt.Errorf("invalid account snapshot")
	}
	for coin, quantity := range value.Positions {
		if coin == "" || !finite(quantity) {
			return value, fmt.Errorf("invalid account position")
		}
	}
	for strategyID, positions := range value.StrategyPositions {
		if strategyID == "" {
			return value, fmt.Errorf("invalid strategy position id")
		}
		for coin, quantity := range positions {
			if coin == "" || !finite(quantity) {
				return value, fmt.Errorf("invalid strategy position")
			}
		}
	}
	return value, nil
}

func DecodeStrategyIntentBatch(raw []byte) (StrategyIntentBatch, error) {
	var value StrategyIntentBatch
	if err := json.Unmarshal(raw, &value); err != nil {
		return value, fmt.Errorf("decode strategy intent batch: %w", err)
	}
	if value.Timestamp == 0 || value.Metadata.SchemaVersion == 0 || value.Metadata.MessageID == "" {
		return value, fmt.Errorf("invalid strategy intent batch")
	}
	return value, nil
}

func DecodeDecisionBatch(raw []byte) (DecisionBatch, error) {
	var value DecisionBatch
	if err := json.Unmarshal(raw, &value); err != nil {
		return value, fmt.Errorf("decode decision batch: %w", err)
	}
	if value.DecisionTimestamp == 0 || value.Metadata.SchemaVersion == 0 || value.Metadata.MessageID == "" {
		return value, fmt.Errorf("invalid decision batch")
	}
	return value, nil
}

func DecodeNotionalPlanningRequest(raw []byte) (NotionalOrderPlanningRequest, error) {
	var value NotionalOrderPlanningRequest
	if err := json.Unmarshal(raw, &value); err != nil {
		return value, fmt.Errorf("decode notional planning request: %w", err)
	}
	if value.DecisionTimestamp == 0 || value.State.StateRevision == 0 || value.Metadata.MessageID == "" {
		return value, fmt.Errorf("invalid notional planning request")
	}
	return value, nil
}

func DecodeNotionalOrderPlan(raw []byte) (NotionalOrderPlanBatch, error) {
	var value NotionalOrderPlanBatch
	if err := json.Unmarshal(raw, &value); err != nil {
		return value, fmt.Errorf("decode notional order plan: %w", err)
	}
	if value.DecisionTimestamp == 0 || value.StateRevision == 0 || value.Metadata.MessageID == "" {
		return value, fmt.Errorf("invalid notional order plan")
	}
	return value, nil
}

func finite(value float64) bool { return !math.IsNaN(value) && !math.IsInf(value, 0) }

type ExchangeOpenOrderSnapshot struct {
	LocalOrderID    uint64  `json:"local_order_id"`
	ExchangeOrderID string  `json:"exchange_order_id"`
	Coin            string  `json:"coin"`
	Side            int     `json:"side"`
	Quantity        float64 `json:"quantity"`
	FilledQuantity  float64 `json:"filled_quantity"`
}

type ExchangeSnapshot struct {
	Timestamp  uint64                      `json:"timestamp"`
	Cash       float64                     `json:"cash"`
	Positions  map[string]float64          `json:"positions"`
	OpenOrders []ExchangeOpenOrderSnapshot `json:"open_orders"`
}

type ExchangeSnapshotEvent struct {
	Metadata ContractMetadata `json:"metadata"`
	Snapshot ExchangeSnapshot `json:"snapshot"`
}

func DecodeExchangeSnapshotEvent(raw []byte) (ExchangeSnapshotEvent, error) {
	var value ExchangeSnapshotEvent
	if err := json.Unmarshal(raw, &value); err != nil {
		return value, fmt.Errorf("decode exchange snapshot event: %w", err)
	}
	if value.Snapshot.Timestamp == 0 || !finite(value.Snapshot.Cash) {
		return value, fmt.Errorf("invalid exchange snapshot event")
	}
	for coin, quantity := range value.Snapshot.Positions {
		if coin == "" || !finite(quantity) {
			return value, fmt.Errorf("invalid exchange position in snapshot")
		}
	}
	for _, order := range value.Snapshot.OpenOrders {
		if order.Coin == "" || (order.Side != 0 && order.Side != 1) || !finite(order.Quantity) || order.Quantity < 0 || !finite(order.FilledQuantity) || order.FilledQuantity < 0 {
			return value, fmt.Errorf("invalid exchange open order in snapshot")
		}
	}
	return value, nil
}
