#include "canonical_market_data_reader.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <unordered_set>


namespace {

std::chrono::year_month_day fromYYYYMMDD(Timestamp value)
{
    const int year = static_cast<int>(value / 10000U);
    const unsigned month = static_cast<unsigned>((value / 100U) % 100U);
    const unsigned day = static_cast<unsigned>(value % 100U);

    const std::chrono::year_month_day result{
        std::chrono::year{year},
        std::chrono::month{month},
        std::chrono::day{day}
    };

    if (!result.ok())
        throw std::invalid_argument("Invalid YYYYMMDD market date");
    return result;
}


Timestamp toYYYYMMDDLocal(std::chrono::year_month_day ymd)
{
    if (!ymd.ok())
        throw std::invalid_argument("Invalid calendar date");
    const int value = int(ymd.year()) * 10000 +
        static_cast<int>(unsigned(ymd.month())) * 100 +
        static_cast<int>(unsigned(ymd.day()));
    return static_cast<Timestamp>(value);
}

bool validBar(const OHLCV& bar)
{
    if (!std::isfinite(bar.open) || bar.open <= 0.0 ||
        !std::isfinite(bar.high) || bar.high <= 0.0 ||
        !std::isfinite(bar.low) || bar.low <= 0.0 ||
        !std::isfinite(bar.close) || bar.close <= 0.0 ||
        !std::isfinite(bar.volume) || bar.volume < 0.0)
        return false;

    return bar.high >= std::max({bar.open, bar.close, bar.low}) &&
           bar.low <= std::min({bar.open, bar.close, bar.high});
}

std::string placeholders(std::size_t count)
{
    std::ostringstream out;
    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0)
            out << ',';
        out << '?';
    }
    return out.str();
}

void requireSqlite(int rc, sqlite3* db, const std::string& operation)
{
    if (rc != SQLITE_OK)
        throw std::runtime_error(operation + ": " + (db ? sqlite3_errmsg(db) : "SQLite error"));
}

} // namespace

CanonicalMarketDataReader::CanonicalMarketDataReader(std::filesystem::path databasePath)
    : database_path_(std::move(databasePath))
{
    if (database_path_.empty())
        throw std::invalid_argument("Canonical market-data database path cannot be empty");
    if (!std::filesystem::exists(database_path_))
        throw std::runtime_error(
            "Canonical market-data database does not exist: " + database_path_.string());

    const int rc = sqlite3_open_v2(
        database_path_.string().c_str(),
        &db_,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
        nullptr
    );

    if (rc != SQLITE_OK) {
        const std::string error = db_ ? sqlite3_errmsg(db_) : "cannot allocate sqlite handle";
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        throw std::runtime_error("Cannot open canonical market-data database read-only: " + error);
    }

    sqlite3_busy_timeout(db_, 5000);
    requireSqlite(
        sqlite3_exec(db_, "PRAGMA query_only=ON;", nullptr, nullptr, nullptr),
        db_,
        "Enable canonical market-data query_only mode failed"
    );
    requireSchema();
}

CanonicalMarketDataReader::~CanonicalMarketDataReader()
{
    if (db_)
        sqlite3_close(db_);
}

void CanonicalMarketDataReader::requireSchema() const
{
    const char* sql =
        "SELECT COUNT(*) FROM sqlite_master "
        "WHERE type='table' AND name IN ('ohlcv_data','market_volume_rank_daily');";

    sqlite3_stmt* stmt = nullptr;
    requireSqlite(sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr), db_, "Prepare schema check failed");

    const int step = sqlite3_step(stmt);
    if (step != SQLITE_ROW) {
        const std::string error = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        throw std::runtime_error("Cannot inspect canonical market-data schema: " + error);
    }

    const int tableCount = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    if (tableCount != 2)
        throw std::runtime_error(
            "Canonical market-data database is missing ohlcv_data or market_volume_rank_daily");
}

bool CanonicalMarketDataReader::hasRankingDate(Timestamp date) const
{
    const char* sql = "SELECT 1 FROM market_volume_rank_daily WHERE date = ? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    requireSqlite(sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr), db_, "Prepare ranking-date query failed");
    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(date));

    const int rc = sqlite3_step(stmt);
    const bool result = rc == SQLITE_ROW;
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        const std::string error = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        throw std::runtime_error("Ranking-date query failed: " + error);
    }

    sqlite3_finalize(stmt);
    return result;
}

std::optional<Timestamp> CanonicalMarketDataReader::latestRankingDateAtOrBefore(Timestamp maxDate) const
{
    if (maxDate == 0)
        throw std::invalid_argument("Canonical ranking frontier max date must be non-zero");

    const char* sql =
        "SELECT MAX(date) FROM market_volume_rank_daily WHERE date <= ?;";
    sqlite3_stmt* stmt = nullptr;
    requireSqlite(
        sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr),
        db_,
        "Prepare canonical ranking frontier query failed"
    );
    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(maxDate));

    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        const std::string error = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        throw std::runtime_error("Canonical ranking frontier query failed: " + error);
    }

    if (sqlite3_column_type(stmt, 0) == SQLITE_NULL) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }

    const sqlite3_int64 value = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    if (value <= 0 || value > static_cast<sqlite3_int64>(maxDate))
        throw std::logic_error("Canonical ranking frontier query returned an invalid date");

    return static_cast<Timestamp>(value);
}

std::vector<Coin> CanonicalMarketDataReader::rankedSymbols(
    Timestamp date,
    unsigned int activeTopN) const
{
    if (date == 0)
        throw std::invalid_argument("Market window date must be non-zero");
    if (activeTopN == 0)
        throw std::invalid_argument("Market top-N must be positive");

    const char* sql =
        "SELECT pair FROM market_volume_rank_daily "
        "WHERE date = ? AND rank <= ? ORDER BY rank ASC;";

    sqlite3_stmt* stmt = nullptr;
    requireSqlite(sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr), db_, "Prepare ranking query failed");
    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(date));
    sqlite3_bind_int(stmt, 2, static_cast<int>(activeTopN));

    std::vector<Coin> result;
    while (true) {
        const int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE)
            break;
        if (rc != SQLITE_ROW) {
            const std::string error = sqlite3_errmsg(db_);
            sqlite3_finalize(stmt);
            throw std::runtime_error("Ranking query failed: " + error);
        }

        const char* pair = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (!pair || std::string(pair).empty()) {
            sqlite3_finalize(stmt);
            throw std::runtime_error("Canonical ranking contains an empty symbol");
        }
        result.emplace_back(pair);
    }

    sqlite3_finalize(stmt);
    if (result.empty())
        throw std::logic_error(
            "Canonical market-data database has no top-N ranking for requested date " +
            std::to_string(date));
    return result;
}

CanonicalMarketDataWindow CanonicalMarketDataReader::loadWindow(
    Timestamp date,
    unsigned int historyDays,
    unsigned int activeTopN,
    const std::set<Coin>& extraHistorySymbols) const
{
    if (historyDays == 0)
        throw std::invalid_argument("Market history window must be positive");

    const std::vector<Coin> ranked = rankedSymbols(date, activeTopN);
    const std::unordered_set<Coin> rankedSet(ranked.begin(), ranked.end());

    std::set<Coin> requestedSymbols(ranked.begin(), ranked.end());
    requestedSymbols.insert(extraHistorySymbols.begin(), extraHistorySymbols.end());

    const auto targetDate = fromYYYYMMDD(date);
    const auto targetSys = std::chrono::sys_days{targetDate};
    const auto startSys = targetSys - std::chrono::days(historyDays - 1);
    const Timestamp startDate = static_cast<Timestamp>(
        toYYYYMMDDLocal(std::chrono::year_month_day{startSys}));

    std::vector<Coin> requested(requestedSymbols.begin(), requestedSymbols.end());
    if (requested.empty())
        throw std::logic_error("Canonical market-data window has no requested symbols");

    const std::string sql =
        "SELECT pair, date, open, high, low, close, volume FROM ohlcv_data "
        "WHERE date BETWEEN ? AND ? AND pair IN (" + placeholders(requested.size()) + ") "
        "ORDER BY pair ASC, date ASC;";

    sqlite3_stmt* stmt = nullptr;
    requireSqlite(sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr), db_, "Prepare OHLCV window query failed");
    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(startDate));
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(date));

    int bindIndex = 3;
    for (const Coin& symbol : requested)
        sqlite3_bind_text(stmt, bindIndex++, symbol.c_str(), -1, SQLITE_TRANSIENT);

    CanonicalMarketDataWindow result;
    result.ranked_symbols = ranked.size();

    while (true) {
        const int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE)
            break;
        if (rc != SQLITE_ROW) {
            const std::string error = sqlite3_errmsg(db_);
            sqlite3_finalize(stmt);
            throw std::runtime_error("OHLCV window query failed: " + error);
        }

        const char* pair = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (!pair) {
            sqlite3_finalize(stmt);
            throw std::runtime_error("Canonical OHLCV row contains NULL pair");
        }

        OHLCV bar;
        bar.open = sqlite3_column_double(stmt, 2);
        bar.high = sqlite3_column_double(stmt, 3);
        bar.low = sqlite3_column_double(stmt, 4);
        bar.close = sqlite3_column_double(stmt, 5);
        bar.volume = sqlite3_column_double(stmt, 6);
        if (!validBar(bar)) {
            sqlite3_finalize(stmt);
            throw std::logic_error(
                "Canonical OHLCV row is invalid for " + std::string(pair) +
                " date=" + std::to_string(sqlite3_column_int64(stmt, 1)));
        }

        const Timestamp rowDate = static_cast<Timestamp>(sqlite3_column_int64(stmt, 1));
        result.raw_data.data[pair][rowDate] = bar;
        ++result.history_rows;
    }

    sqlite3_finalize(stmt);
    result.history_symbols = result.raw_data.data.size();

    CoinBarMap currentBars;
    currentBars.reserve(ranked.size());

    for (const Coin& symbol : ranked) {
        const auto coinIt = result.raw_data.data.find(symbol);
        if (coinIt == result.raw_data.data.end())
            throw std::logic_error(
                "Current top-N symbol has no OHLCV history: " + symbol);

        const auto barIt = coinIt->second.find(date);
        if (barIt == coinIt->second.end())
            throw std::logic_error(
                "Current top-N symbol is missing target-day OHLCV: " + symbol +
                " date=" + std::to_string(date));

        BarData current;
        current.open = barIt->second.open;
        current.high = barIt->second.high;
        current.low = barIt->second.low;
        current.close = barIt->second.close;
        current.volume = barIt->second.volume;
        current.barNumber = static_cast<unsigned int>(coinIt->second.size());
        currentBars.emplace(symbol, current);
    }

    // Extra active-signal symbols are intentionally NOT copied into market_data.
    // Their history is available to IndicatorEngine for exit evaluation, but entry
    // universe selection remains restricted to the current Binance top-N ranking.
    result.market_data.emplace(date, std::move(currentBars));

    for (const Coin& symbol : extraHistorySymbols) {
        if (rankedSet.contains(symbol))
            continue;
        const auto coinIt = result.raw_data.data.find(symbol);
        if (coinIt == result.raw_data.data.end() || coinIt->second.find(date) == coinIt->second.end())
            throw std::logic_error(
                "Active strategy signal is missing target-day OHLCV history: " + symbol +
                " date=" + std::to_string(date));
    }

    return result;
}

MarketData CanonicalMarketDataReader::loadMarketDataWindow(
    Timestamp date,
    unsigned int historyDays,
    const std::set<Coin>& symbols) const
{
    if (date == 0)
        throw std::invalid_argument("Market window date must be non-zero");
    if (historyDays == 0)
        throw std::invalid_argument("Market history window must be positive");
    if (symbols.empty())
        throw std::invalid_argument("Market history symbol set cannot be empty");

    const auto targetDate = fromYYYYMMDD(date);
    const auto targetSys = std::chrono::sys_days{targetDate};
    const auto startSys = targetSys - std::chrono::days(historyDays - 1);
    const Timestamp startDate = static_cast<Timestamp>(
        toYYYYMMDDLocal(std::chrono::year_month_day{startSys}));

    std::vector<Coin> requested(symbols.begin(), symbols.end());
    const std::string sql =
        "SELECT pair, date, open, high, low, close, volume FROM ohlcv_data "
        "WHERE date BETWEEN ? AND ? AND pair IN (" + placeholders(requested.size()) + ") "
        "ORDER BY date ASC, pair ASC;";

    sqlite3_stmt* stmt = nullptr;
    requireSqlite(
        sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr),
        db_,
        "Prepare bounded portfolio OHLCV query failed");
    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(startDate));
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(date));

    int bindIndex = 3;
    for (const Coin& symbol : requested)
        sqlite3_bind_text(stmt, bindIndex++, symbol.c_str(), -1, SQLITE_TRANSIENT);

    MarketData result;
    while (true) {
        const int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE)
            break;
        if (rc != SQLITE_ROW) {
            const std::string error = sqlite3_errmsg(db_);
            sqlite3_finalize(stmt);
            throw std::runtime_error("Bounded portfolio OHLCV query failed: " + error);
        }

        const char* pair = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (!pair || std::string(pair).empty()) {
            sqlite3_finalize(stmt);
            throw std::runtime_error("Canonical OHLCV row contains empty pair");
        }

        OHLCV bar;
        bar.open = sqlite3_column_double(stmt, 2);
        bar.high = sqlite3_column_double(stmt, 3);
        bar.low = sqlite3_column_double(stmt, 4);
        bar.close = sqlite3_column_double(stmt, 5);
        bar.volume = sqlite3_column_double(stmt, 6);
        if (!validBar(bar)) {
            sqlite3_finalize(stmt);
            throw std::logic_error(
                "Canonical OHLCV row is invalid for " + std::string(pair) +
                " date=" + std::to_string(sqlite3_column_int64(stmt, 1)));
        }

        const Timestamp rowDate = static_cast<Timestamp>(sqlite3_column_int64(stmt, 1));
        BarData current;
        current.open = bar.open;
        current.high = bar.high;
        current.low = bar.low;
        current.close = bar.close;
        current.volume = bar.volume;
        current.barNumber = 0;
        result[rowDate].emplace(pair, current);
    }
    sqlite3_finalize(stmt);

    const auto targetIt = result.find(date);
    if (targetIt == result.end())
        throw std::logic_error(
            "Canonical market database has no target-day OHLCV for portfolio-risk date " +
            std::to_string(date));

    for (const Coin& symbol : symbols) {
        if (targetIt->second.find(symbol) == targetIt->second.end())
            throw std::logic_error(
                "Portfolio-risk required symbol is missing target-day OHLCV: " + symbol +
                " date=" + std::to_string(date));
    }

    return result;
}

std::unordered_map<Coin, double> CanonicalMarketDataReader::loadExactClosingPrices(
    Timestamp date,
    const std::set<Coin>& symbols) const
{
    if (date == 0)
        throw std::invalid_argument("Closing-price date must be non-zero");
    if (symbols.empty())
        return {};

    std::vector<Coin> requested(symbols.begin(), symbols.end());
    const std::string sql =
        "SELECT pair, close FROM ohlcv_data WHERE date = ? AND pair IN (" +
        placeholders(requested.size()) + ") ORDER BY pair ASC;";

    sqlite3_stmt* stmt = nullptr;
    requireSqlite(
        sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr),
        db_,
        "Prepare closing-price query failed");
    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(date));

    int bindIndex = 2;
    for (const Coin& symbol : requested)
        sqlite3_bind_text(stmt, bindIndex++, symbol.c_str(), -1, SQLITE_TRANSIENT);

    std::unordered_map<Coin, double> result;
    while (true) {
        const int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE)
            break;
        if (rc != SQLITE_ROW) {
            const std::string error = sqlite3_errmsg(db_);
            sqlite3_finalize(stmt);
            throw std::runtime_error("Closing-price query failed: " + error);
        }

        const char* pair = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const double close = sqlite3_column_double(stmt, 1);
        if (!pair || std::string(pair).empty() || !std::isfinite(close) || close <= 0.0) {
            sqlite3_finalize(stmt);
            throw std::logic_error("Canonical closing-price row is invalid");
        }
        result.emplace(pair, close);
    }
    sqlite3_finalize(stmt);

    for (const Coin& symbol : symbols) {
        if (!result.contains(symbol))
            throw std::logic_error(
                "Canonical market database is missing close(T) for " + symbol +
                " date=" + std::to_string(date));
    }

    return result;
}

