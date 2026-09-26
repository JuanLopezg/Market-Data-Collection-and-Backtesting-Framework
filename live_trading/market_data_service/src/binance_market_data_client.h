#pragma once

#include <cstddef>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "data_types.h"
#include "market_data_store.h"

struct BinanceUniverseSnapshot {
    std::set<std::string> eligible_symbols;
    std::vector<std::pair<std::string, double>> ranked_top;
};

struct BinanceFetchResult {
    OHLCVData bars;
    std::vector<std::string> failed_symbols;
};

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
