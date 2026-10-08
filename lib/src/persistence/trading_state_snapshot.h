#pragma once

#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "data_types.h"
#include "rebalance_plan.h"
#include "execution_order.h"


// Persistable state owned by one StrategyInstance
struct StrategyStateSnapshot {
    StrategyID strategy_id = 0;
    std::unordered_map<Coin, double> signals;
    std::unordered_map<Coin, double> desired_weights;
    std::unordered_map<Coin, double> virtual_positions;
};


// Strategy rebalance plan waiting for a later execution event
struct PendingPlanSnapshot {
    StrategyID strategy_id = 0;
    RebalancePlan plan{0, 0.0};
};


// Minimal operational state required to resume a TradingEngine safely
//
// Analytics are intentionally excluded. Fills are stored as an append-only audit trail
// and can later rebuild analytics independently from operational recovery.
struct TradingStateSnapshot {
    static constexpr unsigned int CURRENT_SCHEMA_VERSION = 1;

    unsigned int schema_version = CURRENT_SCHEMA_VERSION;
    Timestamp last_bar_close_timestamp = 0;
    Timestamp last_execution_timestamp = 0;
    OrderID next_order_id = 1;

    double account_cash = 0.0;
    std::unordered_map<Coin, double> account_positions;

    std::vector<StrategyStateSnapshot> strategies;
    std::vector<PendingPlanSnapshot> pending_plans;
    std::vector<TrackedOrder> orders;
    std::vector<FillID> processed_fill_ids;

    // Schema v1 has no conditional-order fields. Reject before a transaction,
    // rather than dropping a trigger and recovering an unintended market order.
    void requireMarketOnly() const
    {
        for (const auto& order : orders) {
            if (order.request.entry_stop_price != 0.0 || order.request.protective_stop_price != 0.0 || order.request.parent_order_id != 0)
                throw std::invalid_argument("Durable schema v1 does not support stop entries");
        }
        for (const auto& pending : pending_plans) {
            for (const auto& [coin, decision] : pending.plan.values()) {
                (void)coin;
                if (decision.entry_stop_price != 0.0 || decision.protective_stop_price != 0.0)
                    throw std::invalid_argument("Durable schema v1 does not support stop intents");
            }
        }
    }
};
