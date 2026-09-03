#pragma once

#include <unordered_map>
#include <vector>

#include "contract_metadata.h"
#include "rebalance_decision.h"


/**************************************************************************************
 * Type    : StrategyDecisionIntent
 * Purpose : Approved strategy intent produced at close T
 *
 * This deliberately contains weights/reference capital and HOLD/FLAT/TARGET semantics,
 * never final executable quantity. Quantity remains an Execution responsibility at T+1.
 **************************************************************************************/
struct StrategyDecisionIntent {
    StrategyID strategy_id = 0;
    Timestamp decision_timestamp = 0;
    double reference_capital = 0.0;
    std::unordered_map<Coin, RebalanceDecision> decisions;
};


/**************************************************************************************
 * Type    : DecisionBatch
 * Purpose : Atomic decision output for one completed cross-sectional market slice
 **************************************************************************************/
struct DecisionBatch {
    ContractMetadata metadata;
    Timestamp decision_timestamp = 0;
    std::vector<StrategyDecisionIntent> strategies;
};
