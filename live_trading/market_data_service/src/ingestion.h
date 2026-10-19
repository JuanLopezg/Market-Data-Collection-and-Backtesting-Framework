// Declares the daily ingestion coordinator that combines exchange data with canonical SQLite
// storage.

#pragma once

#include <chrono>
#include <cstddef>
#include <functional>

#include "binance_client.h"
#include "config.h"
#include "market_store.h"

struct MarketDataIngestionSummary {
    std::chrono::year_month_day target_date;
    std::size_t ranked_symbols = 0;
    std::size_t active_top_n = 0;
    std::size_t tracked_symbols = 0;
    std::size_t maintained_symbols = 0;
    std::size_t requested_symbols = 0;
    std::size_t downloaded_rows = 0;
    std::unordered_map<Coin, double> paper_opening_prices;
};

// The work of a daily run that needs no closed candle: done a few seconds before midnight
// so the run itself only downloads and commits.
struct MarketDataPreparation {
    std::chrono::year_month_day target_date;
    TrackedMarketData previous;
    // Requests of the previous tracker: nearly always the final plan.
    std::vector<MarketDataDownloadRequest> early_plan;
};

// Coordinates one atomic daily market-data refresh from Binance into canonical storage.
class MarketDataIngestor {
public:
    explicit MarketDataIngestor(MarketDataConfig config);

    // Reads the tracker, builds the expected plan and opens the Binance connection.
    MarketDataPreparation prepare(std::chrono::year_month_day targetDate) const;

    // prepared, when given for the same day, saves the SQLite reads; the tracker is read
    // again inside the run to check it did not change.
    MarketDataIngestionSummary run(std::chrono::year_month_day targetDate,
                                   const MarketDataPreparation* prepared = nullptr);

    // Brings the stored Binance funding settlements of every stored pair up to date, from its
    // first stored candle on (see MarketDataStore::buildFundingPlan). Research data only:
    // never part of the daily checkpoint and never a reason to fail the cycle. Each round
    // commits on its own, so stopping (stopRequested) loses nothing; the next call resumes.
    void refreshFundingRates(const std::function<bool()>& stopRequested) const;

private:
    MarketDataConfig config_;
    BinanceMarketDataClient binance_;

    // Summary of a day committed by an earlier run, for publication after a restart.
    MarketDataIngestionSummary republishStoredDay(
        const MarketDataStore& store,
        const TrackedMarketData& tracked,
        std::chrono::year_month_day targetDate) const;

    TrackedMarketData updateTracker(
        const TrackedMarketData& previous,
        const std::vector<std::pair<std::string, double>>& ranked,
        std::chrono::year_month_day targetDate) const;
};
