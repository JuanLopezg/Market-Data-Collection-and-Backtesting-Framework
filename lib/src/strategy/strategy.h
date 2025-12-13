#pragma once

#include <vector>
#include "data_types.h"   // CoinBarMap, Timestamp, Trade

class Strategy {
public:
    virtual ~Strategy() = default;

    /**********************************************************************************
     * Purpose : Calculate trading signals for the current timestamp.
     * Args    :
     *   - current_trades : Reference to currently open trades (can be modified)
     *   - bars           : Market data for all coins at this timestamp
     *   - ts             : Current timestamp
     **********************************************************************************/
    virtual void calculateSignals(
        std::vector<Trade>& current_trades,
        const CoinBarMap& bars,
        Timestamp ts
    ) = 0;
};
