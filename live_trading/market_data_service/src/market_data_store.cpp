#include "market_data_store.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "service_logging.h"
#include "time_utils.h"

namespace {

using json = nlohmann::json;
constexpr auto EMPTY_DATE = std::chrono::year{2000}/1/1;

std::chrono::year_month_day fromYYYYMMDD(int value)
{
    const int year = value / 10000;
    const unsigned month = static_cast<unsigned>((value / 100) % 100);
    const unsigned day = static_cast<unsigned>(value % 100);
    std::chrono::year_month_day result{
        std::chrono::year{year}, std::chrono::month{month}, std::chrono::day{day}};
    if (!result.ok())
        throw std::runtime_error("Invalid YYYYMMDD value in market-data database");
    return result;
}

std::chrono::year_month_day parseIsoDate(const std::string& text)
{
    if (text.size() != 10 || text[4] != '-' || text[7] != '-')
        throw std::runtime_error("Invalid tracked_pairs date: " + text);

    std::chrono::year_month_day result{
        std::chrono::year{std::stoi(text.substr(0, 4))},
        std::chrono::month{static_cast<unsigned>(std::stoi(text.substr(5, 2)))},
        std::chrono::day{static_cast<unsigned>(std::stoi(text.substr(8, 2)))}
    };
    if (!result.ok())
        throw std::runtime_error("Invalid tracked_pairs date: " + text);
    return result;
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

} // namespace

MarketDataStore::MarketDataStore(std::filesystem::path databasePath)
    : database_path_(std::move(databasePath))
{
    open();
    ensureSchema();
}

MarketDataStore::~MarketDataStore()
{
    if (db_)
        sqlite3_close(db_);
}

void MarketDataStore::open()
{
    if (!database_path_.parent_path().empty())
        std::filesystem::create_directories(database_path_.parent_path());

    if (sqlite3_open(database_path_.string().c_str(), &db_) != SQLITE_OK) {
        const std::string error = db_ ? sqlite3_errmsg(db_) : "cannot allocate sqlite handle";
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        throw std::runtime_error("Cannot open market-data SQLite database: " + error);
    }

    sqlite3_busy_timeout(db_, 5000);
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA synchronous=FULL;");
    exec("PRAGMA foreign_keys=ON;");
}

void MarketDataStore::exec(const char* sql) const
{
    char* error = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string message = error ? error : sqlite3_errmsg(db_);
        if (error)
            sqlite3_free(error);
        throw std::runtime_error("SQLite error: " + message);
    }
}

void MarketDataStore::ensureSchema()
{
    exec(
        "CREATE TABLE IF NOT EXISTS tracked_pairs ("
        "  date TEXT PRIMARY KEY,"
        "  json TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS ohlcv_data ("
        "  pair TEXT NOT NULL,"
        "  date INTEGER NOT NULL,"
        "  open REAL NOT NULL,"
        "  high REAL NOT NULL,"
        "  low REAL NOT NULL,"
        "  close REAL NOT NULL,"
        "  volume REAL NOT NULL,"
        "  PRIMARY KEY(pair, date)"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_ohlcv_date ON ohlcv_data(date);"
        "CREATE TABLE IF NOT EXISTS date_of_start ("
        "  id TEXT PRIMARY KEY"
        ");"
        "CREATE TABLE IF NOT EXISTS market_volume_rank_daily ("
        "  date INTEGER NOT NULL,"
        "  rank INTEGER NOT NULL,"
        "  pair TEXT NOT NULL,"
        "  quote_volume REAL NOT NULL,"
        "  PRIMARY KEY(date, rank),"
        "  UNIQUE(date, pair)"
        ");"
    );
}

TrackedMarketData MarketDataStore::loadTracked() const
{
    TrackedMarketData result;
    result.date = EMPTY_DATE;

    const char* sql = "SELECT date, json FROM tracked_pairs ORDER BY date DESC LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        throw std::runtime_error("Cannot prepare tracked_pairs read: " + std::string(sqlite3_errmsg(db_)));

    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const char* dateText = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* jsonText = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (!dateText || !jsonText) {
            sqlite3_finalize(stmt);
            throw std::runtime_error("tracked_pairs row contains NULL data");
        }

        result.date = parseIsoDate(dateText);
        const json values = json::parse(jsonText);
        if (!values.is_object()) {
            sqlite3_finalize(stmt);
            throw std::runtime_error("tracked_pairs JSON is not an object");
        }
        for (auto it = values.begin(); it != values.end(); ++it)
            result.days_since_top_n[it.key()] = it.value().get<int>();
    }
    else if (rc != SQLITE_DONE) {
        const std::string error = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        throw std::runtime_error("Cannot read tracked_pairs: " + error);
    }

    sqlite3_finalize(stmt);
    return result;
}

std::optional<int> MarketDataStore::latestDateFor(const std::string& symbol) const
{
    const char* sql = "SELECT MAX(date) FROM ohlcv_data WHERE pair = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        throw std::runtime_error("Cannot prepare latest OHLCV query");
    sqlite3_bind_text(stmt, 1, symbol.c_str(), -1, SQLITE_TRANSIENT);

    std::optional<int> result;
    if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL)
        result = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return result;
}

std::optional<int> MarketDataStore::earliestDateFor(const std::string& symbol) const
{
    const char* sql = "SELECT MIN(date) FROM ohlcv_data WHERE pair = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        throw std::runtime_error("Cannot prepare earliest OHLCV query");
    sqlite3_bind_text(stmt, 1, symbol.c_str(), -1, SQLITE_TRANSIENT);

    std::optional<int> result;
    if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL)
        result = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return result;
}

int MarketDataStore::countRowsFor(const std::string& symbol) const
{
    const char* sql = "SELECT COUNT(*) FROM ohlcv_data WHERE pair = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        throw std::runtime_error("Cannot prepare OHLCV count query");
    sqlite3_bind_text(stmt, 1, symbol.c_str(), -1, SQLITE_TRANSIENT);

    int result = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        result = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return result;
}

std::optional<int> MarketDataStore::earliestMissingDateInRecentWindow(
    const std::string& symbol,
    std::chrono::year_month_day targetDate,
    int minimumHistoryDays) const
{
    const auto targetSys = std::chrono::sys_days{targetDate};
    const auto startSys = targetSys - std::chrono::days(minimumHistoryDays - 1);
    const int startValue = toYYYYMMDD(std::chrono::year_month_day{startSys});
    const int endValue = toYYYYMMDD(targetDate);

    const char* sql =
        "SELECT date FROM ohlcv_data "
        "WHERE pair = ? AND date BETWEEN ? AND ? ORDER BY date ASC;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        throw std::runtime_error("Cannot prepare recent OHLCV gap query");
    sqlite3_bind_text(stmt, 1, symbol.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, startValue);
    sqlite3_bind_int(stmt, 3, endValue);

    std::set<int> stored;
    while (sqlite3_step(stmt) == SQLITE_ROW)
        stored.insert(sqlite3_column_int(stmt, 0));
    sqlite3_finalize(stmt);

    // If the symbol has fewer than the desired number of rows, bootstrap is handled
    // by buildDownloadPlan(). Do not classify pre-listing calendar days as corruption here.
    if (countRowsFor(symbol) < minimumHistoryDays)
        return std::nullopt;

    for (auto day = startSys; day <= targetSys; day += std::chrono::days(1)) {
        const int value = toYYYYMMDD(std::chrono::year_month_day{day});
        if (!stored.contains(value))
            return value;
    }
    return std::nullopt;
}

std::vector<MarketDataDownloadRequest> MarketDataStore::buildDownloadPlan(
    const TrackedMarketData& tracked,
    std::chrono::year_month_day targetDate,
    int retentionDays,
    int minimumHistoryDays) const
{
    std::vector<MarketDataDownloadRequest> result;
    const auto targetSys = std::chrono::sys_days{targetDate};

    for (const auto& [symbol, daysSinceTop] : tracked.days_since_top_n) {
        if (daysSinceTop > retentionDays)
            continue;

        const auto latest = latestDateFor(symbol);
        const int rowCount = countRowsFor(symbol);

        std::chrono::year_month_day startDate = targetDate;
        bool needsDownload = false;

        if (!latest.has_value()) {
            startDate = std::chrono::year_month_day{
                targetSys - std::chrono::days(minimumHistoryDays - 1)};
            needsDownload = true;
        }
        else {
            const auto desiredWarmupStart =
                targetSys - std::chrono::days(minimumHistoryDays - 1);
            const auto latestSys = std::chrono::sys_days{fromYYYYMMDD(*latest)};

            if (*latest < toYYYYMMDD(targetDate)) {
                const auto missingForwardStart = latestSys + std::chrono::days(1);

                // If we also have fewer than the minimum warmup rows, request the
                // earlier of the warmup start and the first missing forward day.
                // This both catches up outages and fills the desired recent history.
                const auto chosenStart = rowCount < minimumHistoryDays
                    ? std::min(desiredWarmupStart, missingForwardStart)
                    : missingForwardStart;
                startDate = std::chrono::year_month_day{chosenStart};
                needsDownload = true;
            }
            else if (rowCount < minimumHistoryDays) {
                // Deliberately ask Binance for the desired recent window again. If the
                // instrument is newer than the window, Binance simply returns what exists.
                startDate = std::chrono::year_month_day{desiredWarmupStart};
                needsDownload = true;
            }
            else if (const auto missing = earliestMissingDateInRecentWindow(
                         symbol, targetDate, minimumHistoryDays)) {
                startDate = fromYYYYMMDD(*missing);
                needsDownload = true;
            }
        }

        if (needsDownload && std::chrono::sys_days{startDate} <= targetSys)
            result.push_back({symbol, startDate, targetDate});
    }

    return result;
}

void MarketDataStore::storeTrackedNoTransaction(const TrackedMarketData& tracked)
{
    exec("DELETE FROM tracked_pairs;");

    const char* sql = "INSERT INTO tracked_pairs(date, json) VALUES(?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        throw std::runtime_error("Cannot prepare tracked_pairs insert");

    json payload = json::object();
    for (const auto& [symbol, days] : tracked.days_since_top_n)
        payload[symbol] = days;

    const std::string dateText = formatYMD(tracked.date);
    const std::string jsonText = payload.dump();
    sqlite3_bind_text(stmt, 1, dateText.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, jsonText.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        const std::string error = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        throw std::runtime_error("Cannot store tracked_pairs: " + error);
    }
    sqlite3_finalize(stmt);
}

void MarketDataStore::storeBarsNoTransaction(const OHLCVData& bars)
{
    const char* sql =
        "INSERT INTO ohlcv_data(pair, date, open, high, low, close, volume) "
        "VALUES(?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(pair, date) DO UPDATE SET "
        "open=excluded.open, high=excluded.high, low=excluded.low, "
        "close=excluded.close, volume=excluded.volume;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        throw std::runtime_error("Cannot prepare OHLCV upsert");

    for (const auto& [symbol, byDate] : bars.data) {
        for (const auto& [date, bar] : byDate) {
            if (!validBar(bar)) {
                sqlite3_finalize(stmt);
                throw std::runtime_error("Refusing to persist invalid OHLCV for " + symbol);
            }

            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
            sqlite3_bind_text(stmt, 1, symbol.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 2, static_cast<int>(date));
            sqlite3_bind_double(stmt, 3, bar.open);
            sqlite3_bind_double(stmt, 4, bar.high);
            sqlite3_bind_double(stmt, 5, bar.low);
            sqlite3_bind_double(stmt, 6, bar.close);
            sqlite3_bind_double(stmt, 7, bar.volume);

            if (sqlite3_step(stmt) != SQLITE_DONE) {
                const std::string error = sqlite3_errmsg(db_);
                sqlite3_finalize(stmt);
                throw std::runtime_error("Cannot store OHLCV row: " + error);
            }
        }
    }
    sqlite3_finalize(stmt);
}

void MarketDataStore::storeRankingNoTransaction(
    const std::vector<std::pair<std::string, double>>& rankedTop,
    std::chrono::year_month_day targetDate)
{
    const int dateValue = toYYYYMMDD(targetDate);

    sqlite3_stmt* deleteStmt = nullptr;
    if (sqlite3_prepare_v2(
            db_, "DELETE FROM market_volume_rank_daily WHERE date = ?;", -1,
            &deleteStmt, nullptr) != SQLITE_OK)
        throw std::runtime_error("Cannot prepare daily ranking delete");
    sqlite3_bind_int(deleteStmt, 1, dateValue);
    if (sqlite3_step(deleteStmt) != SQLITE_DONE) {
        const std::string error = sqlite3_errmsg(db_);
        sqlite3_finalize(deleteStmt);
        throw std::runtime_error("Cannot clear daily ranking: " + error);
    }
    sqlite3_finalize(deleteStmt);

    const char* sql =
        "INSERT INTO market_volume_rank_daily(date, rank, pair, quote_volume) "
        "VALUES(?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        throw std::runtime_error("Cannot prepare daily ranking insert");

    int rank = 1;
    for (const auto& [symbol, quoteVolume] : rankedTop) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_int(stmt, 1, dateValue);
        sqlite3_bind_int(stmt, 2, rank++);
        sqlite3_bind_text(stmt, 3, symbol.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 4, quoteVolume);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            const std::string error = sqlite3_errmsg(db_);
            sqlite3_finalize(stmt);
            throw std::runtime_error("Cannot store daily volume ranking: " + error);
        }
    }
    sqlite3_finalize(stmt);
}

void MarketDataStore::commitDailyUpdate(
    const TrackedMarketData& tracked,
    const OHLCVData& bars,
    const std::vector<std::pair<std::string, double>>& rankedTop,
    std::chrono::year_month_day targetDate)
{
    exec("BEGIN IMMEDIATE TRANSACTION;");
    try {
        storeBarsNoTransaction(bars);
        storeRankingNoTransaction(rankedTop, targetDate);
        storeTrackedNoTransaction(tracked);

        // Keep the legacy date_of_start table compatible with existing databases/tools.
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "INSERT OR IGNORE INTO date_of_start(id) VALUES(?);";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
            throw std::runtime_error("Cannot prepare date_of_start insert");
        const std::string dateText = std::to_string(toYYYYMMDD(targetDate));
        sqlite3_bind_text(stmt, 1, dateText.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            const std::string error = sqlite3_errmsg(db_);
            sqlite3_finalize(stmt);
            throw std::runtime_error("Cannot store date_of_start: " + error);
        }
        sqlite3_finalize(stmt);

        exec("COMMIT;");
    }
    catch (...) {
        try { exec("ROLLBACK;"); } catch (...) {}
        throw;
    }
}
