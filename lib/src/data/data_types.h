#include <string>
#include <map>
#include<vector>

#pragma once

using Balance = double;
using Equity = double;
using TradeID = unsigned int;
using Timestamp =  unsigned int;
using Coin = std::string;
enum class Direction {Long,Short,Flat};

struct OHLCV {
    double open;
    double high;
    double low;
    double close;
    double volume;
};

struct OHLCVData {
    std::map<std::string, std::map<unsigned int, OHLCV>> data;
};


struct BarData{
    // OHLCV
    double open;
    double high;
    double low;
    double close;
    double volume;

    unsigned int barNumber = 0;
    double high_20d = 0.0;
    double atr_14d = 0.0;
};


struct Trade{
    TradeID   trade_id_      = 0;
    Timestamp start_         = 0;
    Timestamp end_           = 0;
    double    commission_    = 0.0;
    Coin      coin_          = "";
    Direction direction_     = Direction::Flat;
    double    current_price_ = 0.0;
    double    entry_         = 0.0;
    double    exit_          = 0.0;
    double    size_          = 0.0;
    double    pnl_           = 0.0;
    double    sl_            = 0.0;
    bool      isSimulated_   = true;
    bool      exited_        = false;
    double    slReference_    =0.0; // highest high or lowest low achieved usually for trailling sl
};

using CoinBarMap = std::map<Coin, BarData>;
using EnrichedData =  std::map<Timestamp, CoinBarMap>;

int directionToMultiplier(Direction& dir);

bool hasOpenTrade(const std::vector<Trade>& trades,const Coin& coin);