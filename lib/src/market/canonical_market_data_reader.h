#pragma once

#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <sqlite3.h>

#include "data_types.h"


// Bounded read-only market window for one daily strategy decision
//
// raw_data contains the recent OHLCV history needed to compute indicators.
// market_data contains only the target day's current top-N entry universe.  Extra
// active-signal symbols may be present in raw_data so exit indicators can still be
// evaluated, but they are deliberately excluded from the entry universe.
struct CanonicalMarketDataWindow {
    OHLCVData raw_data;
    MarketData market_data;
    std::size_t ranked_symbols = 0;
    std::size_t history_symbols = 0;
    std::size_t history_rows = 0;
};


// Read bounded LIVE daily history from the canonical SQLite market database
//
// The reader opens SQLite read-only.  The market-data service remains the only writer.
class CanonicalMarketDataReader {
private:
    std::filesystem::path database_path_;
    sqlite3* db_ = nullptr;

    void requireSchema() const;
    std::string quoteVolumeColumn() const;
    std::vector<Coin> rankedSymbols(Timestamp date, unsigned int activeTopN) const;

public:
    explicit CanonicalMarketDataReader(std::filesystem::path databasePath);
    ~CanonicalMarketDataReader();

    CanonicalMarketDataReader(const CanonicalMarketDataReader&) = delete;
    CanonicalMarketDataReader& operator=(const CanonicalMarketDataReader&) = delete;

    bool hasRankingDate(Timestamp date) const;

    // Latest canonical ranking date that is actually persisted and no later than
    // maxDate.  This is the restart/catch-up frontier; callers must not infer
    // canonical availability from simulated time alone.
    std::optional<Timestamp> latestRankingDateAtOrBefore(Timestamp maxDate) const;

    CanonicalMarketDataWindow loadWindow(
        Timestamp date,
        unsigned int historyDays,
        unsigned int activeTopN,
        const std::set<Coin>& extraHistorySymbols = {}
    ) const;

    // Portfolio/risk path: load only explicitly requested symbols. Unlike loadWindow(),
    // this does not depend on the daily Binance ranking and it returns all available
    // rows in MarketData form so covariance/volatility code can use the bounded history.
    MarketData loadMarketDataWindow(
        Timestamp date,
        unsigned int historyDays,
        const std::set<Coin>& symbols
    ) const;

    // Execution/planning path: read the EXACT completed close(T) for a bounded symbol
    // set. This method never falls back to another date. Every requested symbol must
    // exist on T or the call fails. Returned values are reference closes, never fills.
    std::unordered_map<Coin, double> loadExactClosingPrices(
        Timestamp date,
        const std::set<Coin>& symbols
    ) const;

    const std::filesystem::path& databasePath() const { return database_path_; }
};
