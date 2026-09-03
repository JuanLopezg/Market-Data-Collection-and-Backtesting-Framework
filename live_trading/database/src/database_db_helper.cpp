#include "database_downloader.h"
#include "logger.h"
#include "time_utils.h"


#include <sqlite3.h>
#include <algorithm>
#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

static std::chrono::year_month_day fromYYYYMMDD(int yyyymmdd)
{
    int year = yyyymmdd / 10000;
    unsigned month = static_cast<unsigned>((yyyymmdd / 100) % 100);
    unsigned day = static_cast<unsigned>(yyyymmdd % 100);

    return std::chrono::year{year} / std::chrono::month{month} / std::chrono::day{day};
}

/**************************************************************************************
 * Purpose : Opens (or creates) the OHLCV SQLite database and ensures that both:
 *             - tracked_pairs (JSON snapshot table)
 *             - ohlcv_data    (row-per-candle OHLCV table)
 *             - yymmdd        (the first full day of the dataset which is the day it was
 *                                  first written)
 *           exist.
 *
 * Args    : path - Filesystem path to the database file.
 * Return  : sqlite3* - Valid DB handle on success, nullptr on failure.
 **************************************************************************************/
sqlite3* DatabaseDownloader::openDatabaseOHLCV(const boost::filesystem::path& path, std::string yymmdd)
{
    // Ensure directory exists
    if (!path.parent_path().empty() &&
        !boost::filesystem::exists(path.parent_path()))
    {
        boost::filesystem::create_directories(path.parent_path());
    }

    sqlite3* db = nullptr;
    if (sqlite3_open(path.string().c_str(), &db) != SQLITE_OK)
    {
        LG_ERROR("SQLite failed to open DB: {}", sqlite3_errmsg(db));
        sqlite3_close(db);
        return nullptr;
    }

    // Create the 3 tables including date_of_start
    const char* sql =
        "CREATE TABLE IF NOT EXISTS tracked_pairs ("
        "   date TEXT PRIMARY KEY,"
        "   json TEXT NOT NULL"
        ");"

        "CREATE TABLE IF NOT EXISTS ohlcv_data ("
        "   pair TEXT NOT NULL,"
        "   date INTEGER NOT NULL,"
        "   open REAL,"
        "   high REAL,"
        "   low REAL,"
        "   close REAL,"
        "   volume REAL,"
        "   PRIMARY KEY(pair, date)"
        ");"

        "CREATE TABLE IF NOT EXISTS date_of_start ("
        "   id TEXT PRIMARY KEY"
        ");";

    char* errMsg = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &errMsg) != SQLITE_OK)
    {
        LG_ERROR("Database schema creation failed: {}", errMsg);
        sqlite3_free(errMsg);
        sqlite3_close(db);
        return nullptr;
    }

    // --- Check if date_of_start table is empty ---
    sqlite3_stmt* checkStmt = nullptr;
    const char* checkSQL = "SELECT COUNT(*) FROM date_of_start;";

    if (sqlite3_prepare_v2(db, checkSQL, -1, &checkStmt, nullptr) != SQLITE_OK)
    {
        LG_ERROR("Failed to prepare COUNT query: {}", sqlite3_errmsg(db));
        sqlite3_close(db);
        return nullptr;
    }

    bool isEmpty = false;
    if (sqlite3_step(checkStmt) == SQLITE_ROW)
    {
        isEmpty = (sqlite3_column_int(checkStmt, 0) == 0);
    }
    sqlite3_finalize(checkStmt);

    // --- Insert only if empty ---
    if (isEmpty)
    {
        sqlite3_stmt* insertStmt = nullptr;
        const char* insertSQL = "INSERT INTO date_of_start (id) VALUES (?);";

        if (sqlite3_prepare_v2(db, insertSQL, -1, &insertStmt, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_text(insertStmt, 1, yymmdd.c_str(), -1, SQLITE_TRANSIENT);

            if (sqlite3_step(insertStmt) != SQLITE_DONE)
            {
                LG_ERROR("Insert failed for date_of_start: {}", sqlite3_errmsg(db));
            }
        }
        else
        {
            LG_ERROR("Prepare failed for insert: {}", sqlite3_errmsg(db));
        }

        sqlite3_finalize(insertStmt);
    }

    return db;
}


/**************************************************************************************
 * Purpose : Stores new tracked pairs, replacing the single row in tracked_pairs.
 *
 * Args    : db   - SQLite handle.
 *           data - TrackedData to persist.
 *
 * Return  : bool - true on success, false otherwise.
 **************************************************************************************/
bool DatabaseDownloader::storeTrackedPairs(sqlite3* db, const TrackedData& data)
{
    if (!db) return false;

    // Clear old row (tracked_pairs only has ONE row)
    const char* del = "DELETE FROM tracked_pairs;";
    char* errMsg = nullptr;

    if (sqlite3_exec(db, del, nullptr, nullptr, &errMsg) != SQLITE_OK)
    {
        LG_ERROR("Delete failed: {}", errMsg);
        sqlite3_free(errMsg);
        return false;
    }

    const char* insert_sql =
        "INSERT INTO tracked_pairs (date, json) VALUES (?, ?);";

    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db, insert_sql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        LG_ERROR("SQLite prepare failed: {}", sqlite3_errmsg(db));
        return false;
    }

    // --- Format date as YYYY-MM-DD ---
    std::string date_str = formatYMD(data.date);


    // --- Build JSON object ---
    json j = json::object();
    for (auto& [pair, days] : data.trackedPairs)
        j[pair] = days;

    std::string json_str = j.dump();

    sqlite3_bind_text(stmt, 1, date_str.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, json_str.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_DONE)
    {
        LG_ERROR("SQLite insert failed: {}", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return false;
    }

    sqlite3_finalize(stmt);

    LG_INFO("Stored tracked_pairs for {}", date_str);
    return true;
}

/**************************************************************************************
 * Purpose : Stores OHLCV daily candles into the `ohlcv_data` table. Each candle is
 *           stored as one row identified by (pair, date). Uses an UPSERT so calling
 *           this multiple times for the same (pair, date) will overwrite previous
 *           values safely.
 *
 * Args    : db   - SQLite handle (must be valid).
 *           data - OHLCVData containing pair → date → OHLCV.
 *
 * Return  : bool - true on success, false on failure.
 **************************************************************************************/
bool DatabaseDownloader::storeDataOHLCV(sqlite3* db, const OHLCVData& data)
{
    LG_INFO("Storing data ohlcv...");
    if (!db) return false;

    // If there's nothing to store, don't treat it as an error.
    if (data.data.empty()) {
        LG_WARN("No OHLCV data to store.");
        return true;
    }

    const char* sql =
        "INSERT INTO ohlcv_data (pair, date, open, high, low, close, volume) "
        "VALUES (?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(pair, date) DO UPDATE SET "
        "open   = excluded.open, "
        "high   = excluded.high, "
        "low    = excluded.low, "
        "close  = excluded.close, "
        "volume = excluded.volume;";

    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        LG_ERROR("SQLite prepare failed: {}", sqlite3_errmsg(db));
        return false;
    }

    if (sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr) != SQLITE_OK)
    {
        LG_ERROR("SQLite begin transaction failed: {}", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return false;
    }

    // Iterate over all pairs and their daily candles
    for (const auto& [pair, dailyMap] : data.data)
    {
        for (const auto& [yyyymmdd, candle] : dailyMap)
        {
            // Bind parameters: pair, date, open, high, low, close, volume
            sqlite3_bind_text(stmt, 1, pair.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 2, yyyymmdd);
            sqlite3_bind_double(stmt, 3, candle.open);
            sqlite3_bind_double(stmt, 4, candle.high);
            sqlite3_bind_double(stmt, 5, candle.low);
            sqlite3_bind_double(stmt, 6, candle.close);
            sqlite3_bind_double(stmt, 7, candle.volume);

            if (sqlite3_step(stmt) != SQLITE_DONE)
            {
                LG_ERROR("Insert failed: {}", sqlite3_errmsg(db));
                sqlite3_finalize(stmt);
                sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
                return false;
            }

            // Reset statement for next row
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
        }
    }

    sqlite3_finalize(stmt);

    if (sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK)
    {
        LG_ERROR("SQLite commit failed: {}", sqlite3_errmsg(db));
        sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    LG_INFO("Successfully stored OHLCV candles (pairs: {}).",
                 data.data.size());
    return true;
}

/**************************************************************************************
 * Purpose : Prints all OHLCV rows stored for the latest date available in the 
 *           `ohlcv_data` table. This is intended for debugging and verification that 
 *           the daily fetch and storage operations are working correctly.
 *
 * Args    : db - Valid SQLite database handle.
 *
 * Return  : void
 **************************************************************************************/
void DatabaseDownloader::printLatestOHLCV(sqlite3* db)
{
    if (!db) {
        LG_ERROR("DB is null");
        return;
    }

    // ------------------------------------------------------------
    // 1) Find the latest date stored
    // ------------------------------------------------------------
    const char* query_last_date =
        "SELECT MAX(date) FROM ohlcv_data;";

    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db, query_last_date, -1, &stmt, nullptr) != SQLITE_OK)
    {
        LG_ERROR("Prepare failed: {}", sqlite3_errmsg(db));
        return;
    }

    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW)
    {
        LG_WARN("No OHLCV rows found");
        sqlite3_finalize(stmt);
        return;
    }

    int latestDate = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    if (latestDate == 0) {
        LG_WARN("Table empty (MAX(date)=0)");
        return;
    }

    LG_INFO("=== OHLCV DATA FOR LATEST DATE: {} ===", latestDate);

    // ------------------------------------------------------------
    // 2) Query all candles for that date
    // ------------------------------------------------------------
    const char* query_rows =
        "SELECT pair, open, high, low, close, volume "
        "FROM ohlcv_data WHERE date = ? "
        "ORDER BY pair ASC;";

    if (sqlite3_prepare_v2(db, query_rows, -1, &stmt, nullptr) != SQLITE_OK)
    {
        LG_ERROR("Prepare rows failed: {}", sqlite3_errmsg(db));
        return;
    }

    sqlite3_bind_int(stmt, 1, latestDate);

    // ------------------------------------------------------------
    // 3) Iterate results and print
    // ------------------------------------------------------------
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        const char* pair_c = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        double open   = sqlite3_column_double(stmt, 1);
        double high   = sqlite3_column_double(stmt, 2);
        double low    = sqlite3_column_double(stmt, 3);
        double close  = sqlite3_column_double(stmt, 4);
        double volume = sqlite3_column_double(stmt, 5);

        if (!pair_c) continue;

        std::string pair(pair_c);

        LG_INFO(
            "Pair {:<10} | O:{:.4f} H:{:.4f} L:{:.4f} C:{:.4f} V:{:.4f}",
            pair, open, high, low, close, volume
        );
    }

    if (rc != SQLITE_DONE)
        LG_ERROR("Step error: {}", sqlite3_errmsg(db));

    sqlite3_finalize(stmt);

    LG_INFO("=============================================");
}


/**************************************************************************************
 * Purpose : Prints the content of tracked_pairs for diagnostic and debugging purposes.
 * Args    : db - Valid SQLite handle.
 * Return  : void
 **************************************************************************************/
void DatabaseDownloader::printTrackedData(sqlite3* db)
{
    if (!db) {
        LG_ERROR("DB handle is null");
        return;
    }

    const char* sql = "SELECT date, json FROM tracked_pairs LIMIT 1;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        LG_ERROR("SQLite prepare failed: {}", sqlite3_errmsg(db));
        return;
    }

    int rc = sqlite3_step(stmt);

    if (rc == SQLITE_ROW)
    {
        const char* date_c = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* json_c = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));

        if (!date_c || !json_c)
        {
            LG_WARN("No valid row found");
            sqlite3_finalize(stmt);
            return;
        }

        std::string date_str = date_c;
        std::string json_str = json_c;

        LG_INFO("=== TRACKED DATA ===");
        LG_INFO("Date: {}", date_str);

        try {
            json j = json::parse(json_str);

            for (auto it = j.begin(); it != j.end(); ++it)
            {
                LG_INFO("Pair: {:<12} | Days out: {}", it.key(), it.value().get<unsigned>());
            }
        }
        catch (const std::exception& e) {
            LG_ERROR("Failed to parse JSON: {}", e.what());
        }

        LG_INFO("=======================");
    }
    else
    {
        LG_WARN("No tracked_pairs rows found");
    }

    sqlite3_finalize(stmt);
}

/**************************************************************************************
 * Purpose : Prints the OHLCV for BTCUSDT for the latest stored date in the database.
 *
 * Args    : db - Valid SQLite handle.
 * Return  : void
 **************************************************************************************/
void DatabaseDownloader::printLatestBTCUSDT(sqlite3* db)
{
    if (!db) {
        LG_ERROR("DB handle is null");
        return;
    }

    // ------------------------------------------------------------
    // 1) Get the actual latest date stored in ohlcv_data
    // ------------------------------------------------------------
    const char* q_latest =
        "SELECT MAX(date) FROM ohlcv_data;";

    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db, q_latest, -1, &stmt, nullptr) != SQLITE_OK)
    {
        LG_ERROR("Prepare MAX(date) failed: {}", sqlite3_errmsg(db));
        return;
    }

    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        LG_WARN("No OHLCV rows found");
        sqlite3_finalize(stmt);
        return;
    }

    int latestDate = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    if (latestDate == 0) {
        LG_WARN("Table empty (MAX(date)=0)");
        return;
    }

    LG_INFO("=== BTCUSDT OHLCV FOR STORED DATE {} ===", latestDate);

    // ------------------------------------------------------------
    // 2) Query only BTCUSDT for that stored date
    // ------------------------------------------------------------
    const char* q_rows =
        "SELECT open, high, low, close, volume "
        "FROM ohlcv_data "
        "WHERE pair = 'BTCUSDT' AND date = ?;";

    if (sqlite3_prepare_v2(db, q_rows, -1, &stmt, nullptr) != SQLITE_OK)
    {
        LG_ERROR("Prepare rows failed: {}", sqlite3_errmsg(db));
        return;
    }

    sqlite3_bind_int(stmt, 1, latestDate);

    rc = sqlite3_step(stmt);

    if (rc == SQLITE_ROW)
    {
        double open   = sqlite3_column_double(stmt, 0);
        double high   = sqlite3_column_double(stmt, 1);
        double low    = sqlite3_column_double(stmt, 2);
        double close  = sqlite3_column_double(stmt, 3);
        double volume = sqlite3_column_double(stmt, 4);

        LG_INFO("BTCUSDT | O:{:.4f} H:{:.4f} L:{:.4f} C:{:.4f} V:{:.4f}",
                     open, high, low, close, volume);
    }
    else
    {
        LG_WARN("No BTCUSDT row found for stored date {}", latestDate);
    }

    sqlite3_finalize(stmt);
    LG_INFO("=============================================");
}


/**************************************************************************************
 * Purpose : For each tracked pair, determine how many recent OHLCV days need to be
 *           fetched so the last 100-day window has no internal gaps.
 *
 *           Rules:
 *              - If pair has no rows -> return 100
 *              - Only the most recent 100 calendar days are checked/fetched
 *              - If an internal missing date exists, fetch from the earliest missing
 *                date through currentDate so the gap is repaired by the normal UPSERT
 *              - If the whole checked window is complete -> skip the pair
 *
 * Args    : db          - opened SQLite database.
 *           tracked     - TrackedData (contains the map<pair -> days_out>).
 *           currentDate - current date (year_month_day) to compare against.
 *
 * Return  : std::map<std::string,int> -> map of pair -> days to fetch.
 **************************************************************************************/
std::map<std::string,int> DatabaseDownloader::computeDaysSinceLastStoredOHLCV(
    sqlite3* db,
    const TrackedData& tracked,
    std::chrono::year_month_day currentDate
){
    std::map<std::string,int> result;

    if (!db) {
        LG_ERROR("DB is null");
        return result;
    }

    const char* boundsSql =
        "SELECT MIN(date), MAX(date) FROM ohlcv_data WHERE pair = ?;";

    const char* datesSql =
        "SELECT date FROM ohlcv_data "
        "WHERE pair = ? AND date >= ? AND date <= ? "
        "ORDER BY date ASC;";

    sqlite3_stmt* boundsStmt = nullptr;
    sqlite3_stmt* datesStmt = nullptr;

    if (sqlite3_prepare_v2(db, boundsSql, -1, &boundsStmt, nullptr) != SQLITE_OK) {
        LG_ERROR("Prepare bounds query failed: {}", sqlite3_errmsg(db));
        return result;
    }

    if (sqlite3_prepare_v2(db, datesSql, -1, &datesStmt, nullptr) != SQLITE_OK) {
        LG_ERROR("Prepare dates query failed: {}", sqlite3_errmsg(db));
        sqlite3_finalize(boundsStmt);
        return result;
    }

    const auto currentSys = std::chrono::sys_days{currentDate};
    const auto oldestAllowedSys = currentSys - std::chrono::days(99);
    const int currentYMD = toYYYYMMDD(currentDate);

    for (const auto& [pair, daysOut] : tracked.trackedPairs)
    {
        if (daysOut > 0) {
            LG_INFO("{} outside selected ingestion set (days_out={}) -> SKIP",
                         pair, daysOut);
            continue;
        }

        sqlite3_reset(boundsStmt);
        sqlite3_clear_bindings(boundsStmt);
        sqlite3_bind_text(boundsStmt, 1, pair.c_str(), -1, SQLITE_TRANSIENT);

        int rc = sqlite3_step(boundsStmt);
        int firstYMD = 0;
        int lastYMD = 0;

        if (rc == SQLITE_ROW) {
            firstYMD = sqlite3_column_int(boundsStmt, 0);
            lastYMD = sqlite3_column_int(boundsStmt, 1);
        }

        // --------------------------------------------------------------------
        // CASE 1 - never stored -> need full fetch
        // --------------------------------------------------------------------
        if (firstYMD == 0 || lastYMD == 0) {
            result[pair] = 100;
            continue;
        }

        auto firstStoredSys = std::chrono::sys_days{fromYYYYMMDD(firstYMD)};
        auto checkFromSys = std::max(firstStoredSys, oldestAllowedSys);
        int checkFromYMD = toYYYYMMDD(std::chrono::year_month_day{checkFromSys});

        sqlite3_reset(datesStmt);
        sqlite3_clear_bindings(datesStmt);
        sqlite3_bind_text(datesStmt, 1, pair.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(datesStmt, 2, checkFromYMD);
        sqlite3_bind_int(datesStmt, 3, currentYMD);

        std::set<int> storedDates;
        while ((rc = sqlite3_step(datesStmt)) == SQLITE_ROW)
            storedDates.insert(sqlite3_column_int(datesStmt, 0));

        if (rc != SQLITE_DONE) {
            LG_ERROR("Date scan failed for {}: {}", pair, sqlite3_errmsg(db));
            continue;
        }

        std::chrono::sys_days earliestMissingSys{};
        bool hasMissingDate = false;

        for (auto day = checkFromSys; day <= currentSys; day += std::chrono::days(1))
        {
            int ymd = toYYYYMMDD(std::chrono::year_month_day{day});
            if (!storedDates.contains(ymd)) {
                earliestMissingSys = day;
                hasMissingDate = true;
                break;
            }
        }

        // --------------------------------------------------------------------
        // CASE 2 - checked window is complete -> SKIP completely
        // --------------------------------------------------------------------
        if (!hasMissingDate) {
            LG_INFO("{} OHLCV window is complete through {} -> SKIP",
                         pair, currentYMD);
            continue;
        }

        // --------------------------------------------------------------------
        // CASE 3 - fetch from earliest missing day through currentDate
        // --------------------------------------------------------------------
        int daysNeeded = static_cast<int>((currentSys - earliestMissingSys).count()) + 1;
        daysNeeded = std::clamp(daysNeeded, 1, 100);

        int earliestMissingYMD = toYYYYMMDD(
            std::chrono::year_month_day{earliestMissingSys}
        );

        result[pair] = daysNeeded;

        LG_INFO("{} first missing={}, last stored={} -> FETCH {} days",
                     pair, earliestMissingYMD, lastYMD, daysNeeded);
    }

    sqlite3_finalize(datesStmt);
    sqlite3_finalize(boundsStmt);
    return result;
}
