#pragma once

#include <string>
#include <map>
#include <vector>
#include <unordered_map>
#include <limits>


// Type aliases
// Shared types used by strategy, execution, replay and compatibility exports.
using Balance   = double;
using Equity    = double;
using TradeID    = unsigned int;
using OrderID    = unsigned int;
using FillID     = unsigned long long;
using StrategyID = unsigned int;
using Timestamp  = unsigned int; // Daily market keys use YYYYMMDD; this is not Unix milliseconds.
using Coin       = std::string;


// Represents trade/order direction
enum class Direction : int {
    Long  = 1,
    Short = -1,
    Flat  = 0
};


// The Order/Trade types below support the older bar-based interface and exports.
// Current execution uses ExecutionOrder, OrderUpdate and Fill with partial-fill tracking.
// Defines how an order should be filled
//
// Market:
// Fill immediately according to the execution model.
//
// Limit:
// Long  fills when bar.low  <= trigger_price_
// Short fills when bar.high >= trigger_price_
//
// Stop:
// Long  fills when bar.high >= trigger_price_
// Short fills when bar.low  <= trigger_price_
enum class OrderType : int {
    Market = 0,
    Limit  = 1,
    Stop   = 2
};


// Tracks the lifecycle of a pending order
enum class OrderStatus : int {
    Pending   = 0,
    Filled    = 1,
    Cancelled = 2,
    Expired   = 3
};


// Defines how long an order remains valid
enum class OrderTimeInForce : int {
    GoodForBars       = 0,
    GoodTillCancelled = 1
};


// Raw OHLCV price and volume data for a single bar
struct OHLCV {
    double open   = 0.0;
    double high   = 0.0;
    double low    = 0.0;
    double close  = 0.0;
    double volume = 0.0;
    // Actual traded notional in the quote asset; NaN means unavailable in older data.
    double quote_volume = std::numeric_limits<double>::quiet_NaN();
};


// Container for raw OHLCV data indexed by coin and timestamp
//
// Structure :
// data[coin][timestamp] -> OHLCV
struct OHLCVData {
    std::map<Coin, std::map<Timestamp, OHLCV>> data;
};


// Basic market bar used during strategy execution
//
// Important:
// This struct should only contain data intrinsic to the bar itself.
//
// Parameterized indicators such as RSI(14), ATR(20), ROC(5), SMA(Volume, 25),
// Highest(High, 20), etc. should NOT be stored here.
//
// Those are handled by IndicatorEngine.
struct BarData {
    double open   = 0.0;
    double high   = 0.0;
    double low    = 0.0;
    double close  = 0.0;
    double volume = 0.0;

    unsigned int barNumber = 0;
    double quote_volume = std::numeric_limits<double>::quiet_NaN();
};


// Represents an intended entry that may or may not become a real trade
//
// Important:
// An Order is NOT a position.
//
// A pending limit/stop order should not affect balance, equity, exposure, or PnL.
// Only after the order fills should a Trade be created.
//
// Example:
// Strategy creates:
// Buy BTC limit 95000, valid for 1 bar
//
// Next bar:
// If bar.low <= 95000, the order fills and creates a Trade.
// Otherwise, the order expires and disappears.
struct Order {
    OrderID   order_id_        = 0;

    Timestamp created_         = 0;
    Timestamp active_from_     = 0;
    Timestamp filled_          = 0;
    Timestamp cancelled_       = 0;
    Timestamp expired_         = 0;

    Coin      coin_            = "None";
    Direction direction_       = Direction::Flat;

    OrderType type_            = OrderType::Market;
    OrderStatus status_        = OrderStatus::Pending;
    OrderTimeInForce tif_      = OrderTimeInForce::GoodForBars;

    /*
     * For Market orders:
     *   trigger_price_ is ignored.
     *
     * For Limit orders:
     *   trigger_price_ is the limit price.
     *
     * For Stop orders:
     *   trigger_price_ is the stop trigger price.
     */
    double trigger_price_      = 0.0;

    /*
     * Price actually used when the order fills.
     * This is assigned by the execution engine.
     */
    double filled_price_       = 0.0;

    /*
     * Intended position size if filled.
     */
    double size_               = 0.0;

    /*
     * Optional initial stop-loss information copied into the Trade after fill.
     */
    double sl_                 = 0.0;
    double slReference_        = 0.0;

    /*
     * For GoodForBars orders.
     *
     * barsValid_ = 1 means:
     *   active for only the next processed bar.
     */
    unsigned int barsValid_    = 1;
    unsigned int barsAlive_    = 0;

    std::string strategy_name_ = "None";
};


// Represents a single trade lifecycle
//
// Important:
// A Trade means the position already exists.
//
// Pending limit/stop orders should be represented by Order, not by Trade.
struct Trade {
    TradeID   trade_id_        = 0;
    OrderID   source_order_id_ = 0;

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

    /*
     * isSimulated_ should NOT mean "pending order".
     *
     * false = real portfolio trade
     * true  = ignored/debug/non-real trade
     */
    bool      isSimulated_     = true;
    bool      exited_          = false;

    unsigned int barsHeld      = 0;

    std::string strategy_name_ = "None";
};


// Type aliases
// Common containers used across the engine
using CoinBarMap = std::unordered_map<Coin, BarData>;

/*
 * MarketData structure:
 *
 *   marketData[timestamp][coin] -> BarData
 */
using MarketData = std::map<Timestamp, CoinBarMap>;

using OrderList = std::vector<Order>;
using TradeList = std::vector<Trade>;


// Compatibility alias
//
// Previous code used EnrichedData.
// The data is no longer enriched with indicators, but keeping this alias allows
// the rest of the project to be migrated gradually.
using EnrichedData = MarketData;


// Build market data from raw OHLCV data
//
// This function converts:
//
// raw.data[coin][timestamp] -> OHLCV
//
// into:
//
// marketData[timestamp][coin] -> BarData
//
// It only copies raw bar fields and assigns barNumber.
// It does NOT calculate indicators.
MarketData buildMarketData(const OHLCVData& raw);


// Compatibility wrapper
//
// Previous code called buildEnriched().
// This now simply builds basic MarketData.
EnrichedData buildEnriched(const OHLCVData& raw);
