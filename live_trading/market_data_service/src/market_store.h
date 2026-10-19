// Declares canonical SQLite persistence for daily market data and tracked-symbol lifecycle
// state.

#pragma once

#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "data_types.h"

struct TrackedMarketData {
    std::chrono::year_month_day date{std::chrono::year{2000}/1/1};
    std::map<std::string, int> days_since_top_n;
};

struct MarketDataDownloadRequest {
    std::string symbol;
    std::chrono::year_month_day start_date;
    std::chrono::year_month_day end_date;
};

// One Binance funding settlement. rate > 0: longs pay shorts, as a fraction of the position
// notional at mark_price (NaN when Binance did not report it).
struct FundingRateEvent {
    long long funding_time_ms = 0;
    double rate = 0.0;
    double mark_price = 0.0;
};

// Funding settlements of one symbol from start_ms (inclusive) onwards.
struct FundingRateRequest {
    std::string symbol;
    long long start_ms = 0;
};

// SQLite repository for canonical OHLCV and the tracked-symbol lifecycle.
class MarketDataStore {
public:
    explicit MarketDataStore(std::filesystem::path databasePath);
    ~MarketDataStore();

    MarketDataStore(const MarketDataStore&) = delete;
    MarketDataStore& operator=(const MarketDataStore&) = delete;

    const std::filesystem::path& path() const noexcept { return database_path_; }

    TrackedMarketData loadTracked() const;

    // Number of symbols in the stored volume ranking of one completed day.
    int countRankedFor(std::chrono::year_month_day date) const;

    // Symbols with a stored completed candle on this day.
    std::vector<std::string> symbolsWithBarOn(std::chrono::year_month_day date) const;

    std::vector<MarketDataDownloadRequest> buildDownloadPlan(
        const TrackedMarketData& tracked,
        std::chrono::year_month_day targetDate,
        int retentionDays,
        int minimumHistoryDays) const;

    void commitDailyUpdate(
        const TrackedMarketData& tracked,
        const OHLCVData& bars,
        const std::vector<std::pair<std::string, double>>& rankedTop,
        std::chrono::year_month_day targetDate);

    // One request per stored pair whose funding does not reach the settlement that closes
    // its newest stored candle (the next UTC midnight). It starts after the newest stored
    // settlement, or at the pair's first stored candle when it has none.
    std::vector<FundingRateRequest> buildFundingPlan() const;

    // Upserts funding settlements in one transaction. They are research data, kept apart
    // from the daily checkpoint (commitDailyUpdate).
    void storeFundingRates(const std::map<std::string, std::vector<FundingRateEvent>>& eventsBySymbol);

private:
    std::filesystem::path database_path_;
    sqlite3* db_ = nullptr;

    void open();
    void ensureSchema();
    void exec(const char* sql) const;

    std::optional<int> latestDateFor(const std::string& symbol) const;
    std::optional<int> earliestDateFor(const std::string& symbol) const;
    int countRowsFor(const std::string& symbol) const;
    std::optional<int> earliestMissingDateInRecentWindow(
        const std::string& symbol,
        std::chrono::year_month_day targetDate,
        int minimumHistoryDays) const;

    void storeTrackedNoTransaction(const TrackedMarketData& tracked);
    void storeBarsNoTransaction(const OHLCVData& bars);
    void storeRankingNoTransaction(
        const std::vector<std::pair<std::string, double>>& rankedTop,
        std::chrono::year_month_day targetDate);
};
