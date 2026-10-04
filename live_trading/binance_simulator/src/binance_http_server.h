/*
 * File purpose: Declares the small HTTP server that emulates the Binance endpoints used by local market-data tests.
 *
 * Keep this file focused on this responsibility. Trading decisions belong in
 * their domain component; process orchestration belongs in the service application.
 */

#pragma once

#include "historical_market_data.h"

#include <boost/asio/ip/address.hpp>
#include <cstdint>
#include <string>

// Serves the minimal Binance HTTP surface required by local integration tests.
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
