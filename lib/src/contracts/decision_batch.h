#pragma once

#include <unordered_map>
#include <vector>

#include "contract_metadata.h"
#include "rebalance_plan.h"


// Approved strategy intent produced at close T
//
// This contains approved economic targets (USD/notional) plus the original rebalance
// semantics for audit. It never contains an exchange-specific executable quantity.
struct StrategyDecisionIntent {
    StrategyID strategy_id = 0;
    Timestamp decision_timestamp = 0;
    double reference_capital = 0.0;

    // Explicit LIVE economic targets in USD/notional for every non-HOLD decision.
    // Exchange-specific quantity conversion is intentionally deferred downstream.
    std::unordered_map<Coin, double> target_notional_usd;
    std::unordered_map<Coin, RebalanceDecision> decisions;
};


// Atomic decision output for one completed cross-sectional market slice
struct DecisionBatch {
    ContractMetadata metadata;
    Timestamp decision_timestamp = 0;
    std::vector<StrategyDecisionIntent> strategies;
};
