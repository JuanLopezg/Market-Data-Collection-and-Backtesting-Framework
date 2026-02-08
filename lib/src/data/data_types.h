#pragma once

#include <string>
#include <map>
#include <vector>


/**************************************************************************************
 * Type aliases
 * Purpose : Common primitive types used throughout the backtesting engine
 **************************************************************************************/
using Balance   = double;
using Equity    = double;
using TradeID   = unsigned int;
using Timestamp = unsigned int;
using Coin      = std::string;


/**************************************************************************************
 * Type    : Direction
 * Purpose : Represents trade direction
 **************************************************************************************/
enum class Direction : int {
    Long  = 1,
    Short = -1,
    Flat  = 0
};


/**************************************************************************************
 * Type    : OHLCV
 * Purpose : Raw OHLCV price and volume data for a single bar
 **************************************************************************************/
struct OHLCV {
    double open;
    double high;
    double low;
    double close;
    double volume;
};


/**************************************************************************************
 * Type    : OHLCVData
 * Purpose : Container for raw OHLCV data indexed by coin and timestamp
 *
 * Structure :
 *   data[coin][timestamp] -> OHLCV
 **************************************************************************************/
struct OHLCVData {
    std::map<std::string, std::map<unsigned int, OHLCV>> data;
};


/**************************************************************************************
 * Type    : BarData
 * Purpose : Enriched OHLCV data with derived indicators
 *
 * This structure extends raw OHLCV data with precomputed indicators
 * commonly used by strategies (e.g. ATR, rolling highs).
 **************************************************************************************/
struct BarData {
    // Raw OHLCV
    double open;
    double high;
    double low;
    double close;
    double volume;

    // Derived / auxiliary data
    unsigned int barNumber = 0;
    double high_20d        = 0.0;
    double atr_14d         = 0.0;
};


/**************************************************************************************
 * Type    : Trade
 * Purpose : Represents a single trade lifecycle
 *
 * Stores all information related to a trade, including execution details,
 * PnL tracking, stop-loss management, and strategy attribution.
 **************************************************************************************/
struct Trade {
    TradeID   trade_id_        = 0;
    Timestamp start_           = 0;
    Timestamp end_             = 0;
    double    commission_      = 0.0;
    Coin      coin_            = "None";
    Direction direction_       = Direction::Flat;

    double    current_price_   = 0.0;
    double    entry_           = 0.0;
    double    exit_            = 0.0;
    double    size_            = 0.0;
    double    pnl_             = 0.0;

    double    sl_              = 0.0;
    double    slReference_     = 0.0; // highest high / lowest low reached (used for trailing SL)

    bool      isSimulated_     = true;
    bool      exited_          = false;

    std::string strategy_name_ = "None";
};


/**************************************************************************************
 * Type aliases
 * Purpose : Common containers used across the engine
 **************************************************************************************/
using CoinBarMap   = std::map<Coin, BarData>;
using EnrichedData = std::map<Timestamp, CoinBarMap>;
