#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include "data_types.h"

// Last traded price of every Binance USDT perpetual (GET /fapi/v1/ticker/price), used to
// size and, in PAPER, fill a manual rebalance during the day. Public endpoint, no key.
struct LastPrice {
    double price = 0.0;
    std::int64_t time_ms = 0;   // time of that trade, Unix milliseconds
};

class BinanceLastPrices {
public:
    explicit BinanceLastPrices(std::string baseUrl);

    // Throws std::runtime_error when the request or its answer fails.
    std::unordered_map<Coin, LastPrice> fetchAll() const;

private:
    std::string base_url_;
};
