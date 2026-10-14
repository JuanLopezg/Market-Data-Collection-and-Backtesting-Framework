// Declares the Binance REST client used to discover the trading universe and download daily
// candles.

#pragma once

#include <cstddef>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <curl/curl.h>

#include "data_types.h"
#include "market_store.h"

// Result of ranking currently eligible Binance perpetual symbols by quote volume.
struct BinanceUniverseSnapshot {
    std::set<std::string> eligible_symbols;
    std::vector<std::pair<std::string, double>> ranked_top;
    // Binance's own clock when exchangeInfo was answered (Unix milliseconds).
    long long server_time_ms = 0;
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

    // Read only the first price of an already-started daily candle, never its
    // unfinished high/low/close/volume. A candle that has not started yet (no trade
    // since midnight) is polled briefly; a misdated or still missing open fails the cycle.
    std::unordered_map<Coin, double> fetchOpeningPrices(
        std::chrono::year_month_day date,
        const std::vector<std::string>& symbols,
        std::size_t maxParallelRequests) const;

private:
    std::string base_url_;

    // `curl` is a reusable libcurl handle, so consecutive requests share one TLS connection.
    bool httpGet(CURL* curl, const std::string& url, std::string& response, const std::string& name) const;
};
