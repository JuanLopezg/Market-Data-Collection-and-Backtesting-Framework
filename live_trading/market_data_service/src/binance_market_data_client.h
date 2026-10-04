/*
 * File purpose: Declares the Binance REST client used to discover the trading universe and download daily candles.
 *
 * Keep this file focused on this responsibility. Trading decisions belong in
 * their domain component; process orchestration belongs in the service application.
 */

#pragma once

#include <cstddef>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "data_types.h"
#include "market_data_store.h"

// Result of ranking currently eligible Binance perpetual symbols by quote volume.
struct BinanceUniverseSnapshot {
    std::set<std::string> eligible_symbols;
    std::vector<std::pair<std::string, double>> ranked_top;
};

// Daily bars plus the symbols that could not be downloaded successfully.
struct BinanceFetchResult {
    OHLCVData bars;
    std::vector<std::string> failed_symbols;
};

// Small Binance REST adapter used only by the market-data ingestion service.
class BinanceMarketDataClient {
public:
    explicit BinanceMarketDataClient(std::string baseUrl);

    BinanceUniverseSnapshot universeByQuoteVolume(std::size_t rankingLimit) const;

    BinanceFetchResult fetchDailyBars(
        const std::vector<MarketDataDownloadRequest>& requests,
        std::size_t maxParallelRequests) const;

private:
    std::string base_url_;

    bool httpGet(const std::string& url, std::string& response, const std::string& name) const;
};
