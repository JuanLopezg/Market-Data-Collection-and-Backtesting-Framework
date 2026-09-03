#pragma once

#include <vector>

#include "contract_metadata.h"


/**************************************************************************************
 * Type    : MarketBarSnapshot
 * Purpose : One completed OHLCV bar inside a cross-sectional market slice
 **************************************************************************************/
struct MarketBarSnapshot {
    Coin coin;
    OHLCV bar;
};


/**************************************************************************************
 * Type    : MarketSliceSnapshot
 * Purpose : Complete cross-sectional market-data barrier consumed by Decision runtime
 *
 * A message represents one fully closed timestamp. Decision may append it to its local
 * rolling history only as one atomic slice; it must never act on the first individual
 * symbol received for that timestamp.
 **************************************************************************************/
struct MarketSliceSnapshot {
    ContractMetadata metadata;
    Timestamp timestamp = 0;
    std::vector<MarketBarSnapshot> bars;
};
