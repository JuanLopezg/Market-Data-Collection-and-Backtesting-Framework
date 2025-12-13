#include "database_downloader.h"
#include "logger.h"
#include "time_utils.h"

#include <sqlite3.h>
#include <set>
#include <string>

/**************************************************************************************
 * Purpose : Constructs the DatabaseDownloader with the path to the SQLite database.
 * Args    : database_path - Filesystem path to the database file.
 * Return  : None
 **************************************************************************************/
DatabaseDownloader::DatabaseDownloader(boost::filesystem::path database_path)
    : database_path_(std::move(database_path))
{}

/**************************************************************************************
 * Purpose : Debug helper to check which pairs in OHLCVData contain a specific date
 *           (YYYYMMDD) and print their OHLCV for that date.
 *
 * Args    : data      - OHLCVData structure (pair → date → OHLCV)
 *           yyyymmdd  - Target date encoded as an integer in the form YYYYMMDD
 *
 * Return  : void
 **************************************************************************************/
void debugPairsWithDate(const OHLCVData& data, int yyyymmdd)
{
    LG_INFO("=== Checking for date {} ===", yyyymmdd);

    int foundCount = 0;

    for (const auto& [pair, dailyMap] : data.data)
    {
        auto it = dailyMap.find(yyyymmdd);
        if (it == dailyMap.end())
            continue;

        const OHLCV& candle = it->second;
        ++foundCount;

        LG_INFO(
            "Pair {:<12} | {} | O:{:.4f} H:{:.4f} L:{:.4f} C:{:.4f} V:{:.4f}",
            pair,
            yyyymmdd,
            candle.open,
            candle.high,
            candle.low,
            candle.close,
            candle.volume
        );
    }

    if (foundCount == 0)
        LG_WARN("No pairs have data for {}", yyyymmdd);
    else
        LG_INFO("{} pairs have data for {}", foundCount, yyyymmdd);

    LG_INFO("==============================================");
}

void pruneFutureCandles(OHLCVData& data, std::chrono::year_month_day cutoffDate)
{
    int cutoffYMD = toYYYYMMDD(cutoffDate);

    // Iterate over all pairs
    for (auto pairIt = data.data.begin(); pairIt != data.data.end(); )
    {
        auto& dailyMap = pairIt->second;

        // Remove all entries where date > cutoffYMD
        for (auto it = dailyMap.begin(); it != dailyMap.end(); )
        {
            if (it->first > cutoffYMD)
                it = dailyMap.erase(it);   // remove future candle
            else
                ++it;
        }

        // If that pair ended up empty → remove the whole pair
        if (dailyMap.empty())
            pairIt = data.data.erase(pairIt);
        else
            ++pairIt;
    }
}

/**************************************************************************************
 * Purpose : Print ALL OHLCV rows stored in the database for BTCUSDT, ordered by date.
 * Args    : db - valid SQLite handle
 * Return  : void
 **************************************************************************************/
void printAllBTCUSDT(sqlite3* db)
{
    if (!db) {
        LG_ERROR("DB handle is null");
        return;
    }

    const char* sql =
        "SELECT date, open, high, low, close, volume "
        "FROM ohlcv_data "
        "WHERE pair = 'BTCUSDT' "
        "ORDER BY date ASC;";

    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        LG_ERROR("Prepare failed: {}", sqlite3_errmsg(db));
        return;
    }

    LG_INFO("=========== ALL BTCUSDT OHLCV STORED ===========");

    int rowCount = 0;
    int rc;

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        int date     = sqlite3_column_int(stmt, 0);
        double open  = sqlite3_column_double(stmt, 1);
        double high  = sqlite3_column_double(stmt, 2);
        double low   = sqlite3_column_double(stmt, 3);
        double close = sqlite3_column_double(stmt, 4);
        double vol   = sqlite3_column_double(stmt, 5);

        LG_INFO(
            "[{}] O:{:.4f} H:{:.4f} L:{:.4f} C:{:.4f} V:{:.4f}",
            date, open, high, low, close, vol
        );

        rowCount++;
    }

    if (rc != SQLITE_DONE)
        LG_ERROR("Step error: {}", sqlite3_errmsg(db));

    sqlite3_finalize(stmt);

    LG_INFO("=========== {} rows printed ===========", rowCount);
}

void printDateOfStart(sqlite3* db)
{
    const char* sql = "SELECT id FROM date_of_start;";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        LG_ERROR("Failed to prepare SELECT on date_of_start: {}", sqlite3_errmsg(db));
        return;
    }

    std::string allDates;

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const unsigned char* text = sqlite3_column_text(stmt, 0);
        if (text)
        {
            if (!allDates.empty())
                allDates += ", ";   // separator between entries

            allDates += reinterpret_cast<const char*>(text);
        }
    }

    sqlite3_finalize(stmt);

    // Only one final log:
    LG_INFO("date_of_start entries: {}", allDates);
}



/**************************************************************************************
 * Purpose : Main orchestration function called by DatabaseScheduler. This downloads
 *           new tracked data for the given date by:
 *              - Opening the SQLite database
 *              - Loading previous tracked data
 *              - Checking if the database is already up to date
 *              - Fetching the top-50 Binance perpetual pairs
 *              - Computing updated tracked-pair day counts
 *              - Storing results in the database
 *              - Printing resulting tracked_pairs contents
 *
 * Args    : date - The UTC date for which tracked data should be computed.
 * Return  : bool - true on success, false otherwise.
 **************************************************************************************/
bool DatabaseDownloader::downloadData(std::chrono::year_month_day date)
{
    std::string date_str = formatYMD(date);
    LG_INFO("DownloadData({})", date_str);

    // ------------------------------------------------------------
    // Open database
    // ------------------------------------------------------------
    sqlite3* db = openDatabaseOHLCV(database_path_,std::to_string(toYYYYMMDD(date)));
    if (!db)
    {
        LG_ERROR("Could not open database {}", database_path_.string());
        return false;
    }
    printDateOfStart(db);

    // ------------------------------------------------------------
    // Load previous tracked data (if any)
    // ------------------------------------------------------------
    TrackedData prev = getTrackedPairs(db);
    bool prev_exists = prev.date != EMPTY_DATE && !prev.trackedPairs.empty();

    TrackedData updated_tracked_data = prev;

    // only if data for todays fetch didn't exist
    if(!prev_exists || prev.date != date){
        // ------------------------------------------------------------
        // Always compute updated tracked pairs, even if empty
        // ------------------------------------------------------------
        LG_INFO("Fetching top-50 volume pairs...");
        std::set<std::string> top50 = getTop50PairsByVolume();

        if (top50.empty())
        {
            LG_ERROR("ERROR — top-50 list is empty. Aborting.");
            sqlite3_close(db);
            return false;
        }

        // Compute new tracked-pairs state for "date"
        updated_tracked_data = getNewTrackedPairs(prev, top50, prev_exists, date);

        // ------------------------------------------------------------
        // Store TRACKED PAIRS FIRST (always)
        // ------------------------------------------------------------
        if (!storeTrackedPairs(db, updated_tracked_data))
        {
            LG_ERROR("Failed to store tracked pairs.");
            sqlite3_close(db);
            return false;
        }

        LG_INFO("Tracked_pairs updated for {}", date_str);

    }else{
        LG_INFO("Tracked_pairs already up to date for {}", date_str);
    }

    std::map<std::string,int> dataToDownload = computeDaysSinceLastStoredOHLCV(db,updated_tracked_data, date);

    if(dataToDownload.empty()){
        LG_INFO("OOHLCV already up to date.");
        sqlite3_close(db);
        return false;
    }

    // ------------------------------------------------------------
    // NOW fetch OHLCV data — even if tracked_pairs was already up-to-date
    // ------------------------------------------------------------
    OHLCVData data_ohlcv = fetchDataOHLCV(date, dataToDownload);

    pruneFutureCandles(data_ohlcv, date);

    if (data_ohlcv.data.empty())
    {
        LG_WARN("No OHLCV data returned for {}", date_str);
        printTrackedData(db);
        sqlite3_close(db);
        return true;                       // Not an error — tracked pairs updated anyway
    }

    // ------------------------------------------------------------
    // Store OHLCV data
    // ------------------------------------------------------------
    if (!storeDataOHLCV(db, data_ohlcv))
    {
        LG_ERROR("Failed to store OHLCV data.");
        sqlite3_close(db);
        return false;
    }

    LG_INFO("OHLCV data stored for {}", date_str);

    printTrackedData(db);
    printAllBTCUSDT(db);
    sqlite3_close(db);

    LG_INFO("Database updated successfully.");

    return true;
}
