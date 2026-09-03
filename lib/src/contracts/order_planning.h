#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "contract_metadata.h"
#include "decision_batch.h"
#include "execution_order.h"
#include "execution_price_snapshot.h"
#include "tracked_order.h"


/**************************************************************************************
 * Type    : ExecutionPlanningStateSnapshot
 * Purpose : Immutable execution-state input required to create an order plan
 *
 * The planner does not own this state. A reconciliation/execution-state authority
 * publishes it and later validates/applies the returned plan before exchange commands
 * are emitted.
 **************************************************************************************/
struct ExecutionPlanningStateSnapshot {
    std::uint64_t state_revision = 0;
    std::vector<StrategyID> strategy_ids;
    std::unordered_map<StrategyID, std::unordered_map<Coin, double>> strategy_positions;
    std::vector<TrackedOrder> orders;
    OrderID next_order_id = 1;
};


/**************************************************************************************
 * Type    : OrderPlanningRequest
 * Purpose : Self-contained deterministic request for close-T intent at open T+1
 **************************************************************************************/
struct OrderPlanningRequest {
    ContractMetadata metadata;
    Timestamp decision_timestamp = 0;
    Timestamp execution_timestamp = 0;
    DecisionBatch decisions;
    ExecutionPriceSnapshot prices;
    ExecutionPlanningStateSnapshot state;
};


/**************************************************************************************
 * Type    : OrderPlanBatch
 * Purpose : Pure planner output; no exchange side effects have happened yet
 *
 * next_order_id is the value that the execution-state authority must persist if this
 * plan is accepted. global_target_exposure is included for audit/comparison only.
 **************************************************************************************/
struct OrderPlanBatch {
    ContractMetadata metadata;
    Timestamp decision_timestamp = 0;
    Timestamp execution_timestamp = 0;
    std::uint64_t state_revision = 0;
    DecisionBatch decisions;
    ExecutionPriceSnapshot prices;
    OrderID next_order_id = 1;
    std::vector<OrderID> cancel_order_ids;
    std::vector<ExecutionOrder> submit_orders;
    std::unordered_map<Coin, double> global_target_exposure;
};
