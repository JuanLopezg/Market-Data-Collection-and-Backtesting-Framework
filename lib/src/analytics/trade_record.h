#pragma once

#include <string>

#include "data_types.h"


/**************************************************************************************
 * Type    : TradeRecord
 * Purpose : Analytics representation reconstructed from strategy-attributed fills
 *
 * A TradeRecord is not live state. Positions/Account remain the source of truth.
 **************************************************************************************/
struct TradeRecord {
    TradeID trade_id = 0;
    StrategyID strategy_id = 0;
    std::string strategy_name;

    Coin coin;
    Direction direction = Direction::Flat;

    Timestamp start = 0;
    Timestamp end = 0;

    // Campaign boundary prices: first opening fill and final closing fill.
    // Intermediate rebalance fills do not change either value.
    double entry_price = 0.0;
    double exit_price = 0.0;
    double peak_quantity = 0.0;

    double commission = 0.0;
    double pnl = 0.0;

    unsigned int fill_count = 0;
    bool exited = false;
};
