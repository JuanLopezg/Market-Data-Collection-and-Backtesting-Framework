#pragma once

#include <string>
#include <map>
#include <vector>
#include <unordered_map>


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
    double open   = 0.0;
    double high   = 0.0;
    double low    = 0.0;
    double close  = 0.0;
    double volume = 0.0;
};


/**************************************************************************************
 * Type    : OHLCVData
 * Purpose : Container for raw OHLCV data indexed by coin and timestamp
 *
 * Structure :
 *   data[coin][timestamp] -> OHLCV
 **************************************************************************************/
struct OHLCVData {
    std::map<Coin, std::map<Timestamp, OHLCV>> data;
};


/**************************************************************************************
 * Type    : BarData
 * Purpose : Basic market bar used during strategy execution
 *
 * Important:
 *   This struct should only contain data intrinsic to the bar itself.
 *
 *   Parameterized indicators such as RSI(14), ATR(20), ROC(5), SMA(Volume, 25),
 *   Highest(High, 20), etc. should NOT be stored here.
 *
 *   Those are now handled by IndicatorEngine.
 **************************************************************************************/
struct BarData {
    double open   = 0.0;
    double high   = 0.0;
    double low    = 0.0;
    double close  = 0.0;
    double volume = 0.0;

    unsigned int barNumber = 0;
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
    double    slReference_     = 0.0;

    bool      isSimulated_     = true;
    bool      exited_          = false;

    unsigned int barsHeld      = 0;

    std::string strategy_name_ = "None";
};


/**************************************************************************************
 * Type aliases
 * Purpose : Common containers used across the engine
 **************************************************************************************/
using CoinBarMap = std::unordered_map<Coin, BarData>;

/*
 * MarketData structure:
 *
 *   marketData[timestamp][coin] -> BarData
 */
using MarketData = std::map<Timestamp, CoinBarMap>;


/**************************************************************************************
 * Compatibility alias
 *
 * Previous code used EnrichedData.
 * The data is no longer enriched with indicators, but keeping this alias allows
 * the rest of the project to be migrated gradually.
 **************************************************************************************/
using EnrichedData = MarketData;


/**************************************************************************************
 * Purpose : Build market data from raw OHLCV data
 *
 * This function converts:
 *
 *   raw.data[coin][timestamp] -> OHLCV
 *
 * into:
 *
 *   marketData[timestamp][coin] -> BarData
 *
 * It only copies raw bar fields and assigns barNumber.
 * It does NOT calculate indicators.
 **************************************************************************************/
MarketData buildMarketData(const OHLCVData& raw);


/**************************************************************************************
 * Compatibility wrapper
 *
 * Previous code called buildEnriched().
 * This now simply builds basic MarketData.
 **************************************************************************************/
EnrichedData buildEnriched(const OHLCVData& raw);