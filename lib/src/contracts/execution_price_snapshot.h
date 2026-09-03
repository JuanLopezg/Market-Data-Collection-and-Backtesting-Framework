#pragma once

#include <unordered_map>

#include "contract_metadata.h"


/**************************************************************************************
 * Type    : ExecutionPriceSnapshot
 * Purpose : Executable/reference prices observed at execution time (for example T+1 open)
 **************************************************************************************/
struct ExecutionPriceSnapshot {
    ContractMetadata metadata;
    Timestamp timestamp = 0;
    Timestamp decision_timestamp = 0;
    std::unordered_map<Coin, double> prices;
};
