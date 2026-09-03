#pragma once

#include <unordered_map>

#include "contract_metadata.h"


/**************************************************************************************
 * Type    : AccountSnapshot
 * Purpose : Read-only business-state projection owned by ExecutionEngine
 **************************************************************************************/
struct AccountSnapshot {
    ContractMetadata metadata;
    Timestamp timestamp = 0;
    double cash = 0.0;
    std::unordered_map<Coin, double> positions;
    std::unordered_map<StrategyID, std::unordered_map<Coin, double>> strategy_positions;
};
