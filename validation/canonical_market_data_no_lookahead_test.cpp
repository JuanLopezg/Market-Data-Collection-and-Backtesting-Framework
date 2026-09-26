#include <cassert>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

#include <sqlite3.h>

#include "canonical_market_data_reader.h"

namespace {

void exec(sqlite3* db, const char* sql)
{
    char* error = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string message = error ? error : sqlite3_errmsg(db);
        if (error)
            sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

void buildFixture(const std::filesystem::path& path)
{
    std::filesystem::remove(path);
    sqlite3* db = nullptr;
    if (sqlite3_open(path.string().c_str(), &db) != SQLITE_OK)
        throw std::runtime_error("cannot create SQLite fixture");

    try {
        exec(db,
            "CREATE TABLE ohlcv_data("
            "pair TEXT NOT NULL,date INTEGER NOT NULL,open REAL NOT NULL,high REAL NOT NULL,"
            "low REAL NOT NULL,close REAL NOT NULL,volume REAL NOT NULL,PRIMARY KEY(pair,date));"
            "CREATE TABLE market_volume_rank_daily("
            "date INTEGER NOT NULL,rank INTEGER NOT NULL,pair TEXT NOT NULL,quote_volume REAL NOT NULL,"
            "PRIMARY KEY(date,rank),UNIQUE(date,pair));"

            // T-1, T and T+1 are deliberately all present. The reader must never leak T+1
            // when the caller asks for an economic decision at T.
            "INSERT INTO ohlcv_data VALUES"
            "('BTC',20200101,10,12,9,11,100),"
            "('BTC',20200102,11,13,10,12,110),"
            "('BTC',20200103,99,101,98,100,999),"
            "('ETH',20200101,20,22,19,21,200),"
            "('ETH',20200102,21,23,20,22,210),"
            "('ETH',20200103,199,201,198,200,1999),"
            "('ACTIVE',20200101,30,32,29,31,50),"
            "('ACTIVE',20200102,31,33,30,32,55),"
            "('ACTIVE',20200103,299,301,298,300,2999),"
            "('FUTURE',20200103,400,402,399,401,5000);"

            "INSERT INTO market_volume_rank_daily VALUES"
            "(20200102,1,'BTC',110),"
            "(20200102,2,'ETH',210),"
            "(20200103,1,'FUTURE',5000),"
            "(20200103,2,'BTC',999);"
        );
        sqlite3_close(db);
    }
    catch (...) {
        sqlite3_close(db);
        throw;
    }
}

void assertNoFutureRaw(const OHLCVData& raw, Timestamp cutoff)
{
    for (const auto& [symbol, rows] : raw.data) {
        (void)symbol;
        for (const auto& [date, bar] : rows) {
            (void)bar;
            if (date > cutoff)
                throw std::runtime_error("raw_data leaked a future row");
        }
    }
}

void assertNoFutureMarket(const MarketData& data, Timestamp cutoff)
{
    for (const auto& [date, bars] : data) {
        (void)bars;
        if (date > cutoff)
            throw std::runtime_error("MarketData leaked a future row");
    }
}

} // namespace

int main()
{
    const auto path = std::filesystem::temp_directory_path() / "algotrading_t16_no_lookahead.db";
    buildFixture(path);

    try {
        CanonicalMarketDataReader reader(path);
        constexpr Timestamp T = 20200102;

        // Strategy path: ranking must be exactly T, history must be bounded <= T, and
        // a future-ranked-only symbol must not enter the current universe.
        const CanonicalMarketDataWindow strategy = reader.loadWindow(
            T, 2, 2, std::set<Coin>{"ACTIVE"});
        assertNoFutureRaw(strategy.raw_data, T);
        assertNoFutureMarket(strategy.market_data, T);
        if (strategy.market_data.size() != 1 || strategy.market_data.begin()->first != T)
            throw std::runtime_error("strategy target market_data is not exactly T");
        const auto& current = strategy.market_data.begin()->second;
        if (!current.contains("BTC") || !current.contains("ETH") || current.contains("FUTURE"))
            throw std::runtime_error("strategy universe used a ranking from the future");
        if (!strategy.raw_data.data.contains("ACTIVE"))
            throw std::runtime_error("active-signal bounded history missing");
        if (strategy.raw_data.data.at("ACTIVE").contains(20200103))
            throw std::runtime_error("active-signal history leaked T+1");

        // PortfolioRisk path: even though SQLite physically contains T+1, the bounded
        // risk lookback must end at T.
        const MarketData risk = reader.loadMarketDataWindow(
            T, 3, std::set<Coin>{"BTC", "ETH"});
        assertNoFutureMarket(risk, T);
        if (!risk.contains(T) || risk.contains(20200103))
            throw std::runtime_error("portfolio-risk window crossed the decision boundary");

        // Execution/planning path: exact close(T), never T+1 or a fallback.
        const auto closes = reader.loadExactClosingPrices(T, std::set<Coin>{"BTC", "ETH"});
        if (std::abs(closes.at("BTC") - 12.0) > 1e-12 ||
            std::abs(closes.at("ETH") - 22.0) > 1e-12)
            throw std::runtime_error("execution reference close is not exact close(T)");

        // Ranking lookup itself is date-scoped.
        if (!reader.hasRankingDate(T) || !reader.hasRankingDate(20200103))
            throw std::runtime_error("fixture ranking dates missing");

        std::filesystem::remove(path);
        std::cout << "PASS: T16 canonical market-data no-lookahead unit tests\n";
        return 0;
    }
    catch (...) {
        std::filesystem::remove(path);
        throw;
    }
}
