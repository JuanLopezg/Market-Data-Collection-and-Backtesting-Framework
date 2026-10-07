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

// SQLite repository for canonical OHLCV and the tracked-symbol lifecycle.
class MarketDataStore {
public:
    explicit MarketDataStore(std::filesystem::path databasePath);
    ~MarketDataStore();

    MarketDataStore(const MarketDataStore&) = delete;
    MarketDataStore& operator=(const MarketDataStore&) = delete;

    const std::filesystem::path& path() const noexcept { return database_path_; }

    TrackedMarketData loadTracked() const;

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
