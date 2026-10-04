/*
 * File purpose: Declares the daily ingestion coordinator that combines exchange data with canonical SQLite storage.
 *
 * Keep this file focused on this responsibility. Trading decisions belong in
 * their domain component; process orchestration belongs in the service application.
 */

#pragma once

#include <chrono>
#include <cstddef>

#include "binance_market_data_client.h"
#include "market_data_config.h"
#include "market_data_store.h"

struct MarketDataIngestionSummary {
    std::chrono::year_month_day target_date;
    std::size_t ranked_symbols = 0;
    std::size_t active_top_n = 0;
    std::size_t tracked_symbols = 0;
    std::size_t maintained_symbols = 0;
    std::size_t requested_symbols = 0;
    std::size_t downloaded_rows = 0;
};

// Coordinates one atomic daily market-data refresh from Binance into canonical storage.
class MarketDataIngestor {
public:
    explicit MarketDataIngestor(MarketDataConfig config);

    MarketDataIngestionSummary run(std::chrono::year_month_day targetDate);

private:
    MarketDataConfig config_;
    BinanceMarketDataClient binance_;

    TrackedMarketData updateTracker(
        const TrackedMarketData& previous,
        const std::vector<std::pair<std::string, double>>& ranked,
        std::chrono::year_month_day targetDate) const;
};
