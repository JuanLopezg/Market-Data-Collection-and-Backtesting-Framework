#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "daily_close_snapshot.h"
#include "decision_batch.h"
#include "execution_order.h"
#include "order_planning.h"


/**************************************************************************************
 * Type    : PlannedNotionalOrder
 * Purpose : Exchange-agnostic economic order intent, still expressed in USD notional
 *
 * Quantity conversion is deliberately deferred to the exchange-specific edge. The
 * future venue adapter converts notional_usd / close(T) to a venue-valid quantity and
 * applies symbol/precision/min-size rules. Actual fill price remains exchange truth.
 **************************************************************************************/
struct PlannedNotionalOrder {
    // Deterministic logical identity for this economic order intent. This is NOT yet
    // a venue/exchange order id. Step 7 derives venue/client identity from this value.
    std::string economic_order_id;

    OrderID order_id = 0;
    StrategyID strategy_id = 0;
    Timestamp created_at = 0;
    Timestamp decision_timestamp = 0;
    std::uint64_t state_revision = 0;
    Coin coin;
    OrderSide side = OrderSide::Buy;

    // Audit trail for the exact close(T)-based economic calculation.
    double reference_close = 0.0;
    double target_notional_usd = 0.0;
    double current_notional_usd = 0.0;
    double pending_notional_usd = 0.0;
    double delta_notional_usd = 0.0; // signed: buy > 0, sell < 0

    // Absolute requested economic order size. Kept explicit for the Step 7 adapter.
    double notional_usd = 0.0;
};


/**************************************************************************************
 * Type    : NotionalOrderPlanningRequest
 * Purpose : Self-contained LIVE planning request for one completed UTC day T
 **************************************************************************************/
struct NotionalOrderPlanningRequest {
    ContractMetadata metadata;
    Timestamp decision_timestamp = 0;
    DecisionBatch decisions;
    DailyCloseSnapshot reference_closes;
    ExecutionPlanningStateSnapshot state;
};


/**************************************************************************************
 * Type    : NotionalOrderPlanBatch
 * Purpose : Planner output that stops at the USD/notional boundary
 *
 * No exchange order has been submitted and no exchange-specific quantity has been
 * chosen. Step 7 will consume this boundary and perform venue-specific conversion.
 **************************************************************************************/
struct NotionalOrderPlanBatch {
    ContractMetadata metadata;
    Timestamp decision_timestamp = 0;
    std::uint64_t state_revision = 0;
    DecisionBatch decisions;
    DailyCloseSnapshot reference_closes;
    OrderID next_order_id = 1;
    std::vector<OrderID> cancel_order_ids;
    std::vector<PlannedNotionalOrder> submit_orders;
    std::unordered_map<Coin, double> global_target_notional_usd;
};
