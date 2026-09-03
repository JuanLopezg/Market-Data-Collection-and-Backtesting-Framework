#pragma once

#include "historical_market_data.h"

#include <boost/asio/ip/address.hpp>
#include <cstdint>
#include <string>

class BinanceHttpServer {
public:
    BinanceHttpServer(
        HistoricalMarketData& marketData,
        boost::asio::ip::address address,
        std::uint16_t port);

    void run();

private:
    HistoricalMarketData& marketData_;
    boost::asio::ip::address address_;
    std::uint16_t port_ = 0;
};
