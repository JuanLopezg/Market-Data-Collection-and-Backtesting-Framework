#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "contract_metadata.h"


/**************************************************************************************
 * Type    : StrategySignalIntent
 * Purpose : Complete signal snapshot produced by one strategy at a closed timestamp
 *
 * Signals are opinions only and must stay inside [-1,+1]. Missing coins mean zero.
 * This contract deliberately contains no sizing, capital, risk, target weights, orders
 * or execution quantities.
 **************************************************************************************/
struct StrategySignalIntent {
    StrategyID strategy_id = 0;
    std::string strategy_name;
    std::unordered_map<Coin, double> signals;
};


/**************************************************************************************
 * Type    : StrategyIntentBatch
 * Purpose : Atomic cross-strategy signal output for one complete market slice
 **************************************************************************************/
struct StrategyIntentBatch {
    ContractMetadata metadata;
    Timestamp timestamp = 0;
    std::vector<StrategySignalIntent> strategies;
};
