#pragma once

#include <vector>

#include "contract_metadata.h"


/**************************************************************************************
 * Type    : MarketSliceClosed
 * Purpose : Barrier event emitted only after one complete cross-sectional market slice
 *
 * DecisionEngine must consume this event as a slice boundary rather than acting on the
 * first individual symbol bar. This preserves cross-sectional universes/rankers.
 **************************************************************************************/
struct MarketSliceClosed {
    ContractMetadata metadata;
    Timestamp timestamp = 0;
    std::vector<Coin> symbols;
};
