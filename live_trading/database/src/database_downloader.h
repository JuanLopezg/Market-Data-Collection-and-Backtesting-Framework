#pragma once

#include <boost/filesystem.hpp>
#include <map>
#include <string>
#include <set>
#include <chrono>
#include <sqlite3.h>
#include "data_types.h"

/***********************************************
 * A placeholder date used when database has
 * no stored tracked_pairs.
 ***********************************************/
static constexpr std::chrono::year_month_day EMPTY_DATE =
    std::chrono::year{2000}/1/1;

/***********************************************
 * Struct storing a UTC date and a mapping
 * of trading pairs → days outside top-50.
 ***********************************************/
struct TrackedData {
    std::chrono::year_month_day date;      // Stored date for these stats
    std::map<std::string, int> trackedPairs; // Map pair → days outside top-50
};

/***********************************************
 * Main downloader class performing:
 *  - SQLite database operations
 *  - Binance API data retrieval
 *  - Tracking logic recomputing pair counters
 ***********************************************/
class DatabaseDownloader {
public:
    /**************************************************************************************
     * Purpose : Construct the downloader with the path to an SQLite DB file.
     * Args    : database_path - Filesystem path to the OHLCV database.
     **************************************************************************************/
    DatabaseDownloader(boost::filesystem::path database_path);
    ~DatabaseDownloader() = default;

    /**************************************************************************************
     * Purpose : Main orchestration function invoked once per day by DatabaseScheduler.
     * Args    : date - The UTC date for which data must be computed and stored.
     * Return  : bool - true on success, false on failure.
     **************************************************************************************/
    bool downloadData(std::chrono::year_month_day date);

private:
    // Path to the database file
    boost::filesystem::path database_path_;

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
    sqlite3* openDatabaseOHLCV(const boost::filesystem::path& path, std::string yymmdd);

    /**************************************************************************************
     * Purpose : Read the only row of tracked_pairs in the database.
     * Args    : db - SQLite handle.
     * Return  : TrackedData - Parsed struct. If empty, date == EMPTY_DATE.
     **************************************************************************************/
    TrackedData getTrackedPairs(sqlite3* db);

    /**************************************************************************************
     * Purpose : Store (overwrite) tracked_pairs in the SQLite database.
     * Args    : db   - SQLite handle.
     *           data - TrackedData to write.
     * Return  : bool - true on success.
     **************************************************************************************/
    bool storeTrackedPairs(sqlite3* db, const TrackedData& data);

    /**************************************************************************************
     * Purpose : Print tracked_pairs content for debugging/logging.
     * Args    : db - SQLite database handle.
     **************************************************************************************/
    void printTrackedData(sqlite3* db);

    /**************************************************************************************
     * Purpose : Fetch the top-50 Binance USDT perpetual futures pairs by 24h volume.
     * Return  : std::set<std::string> - Set of pair symbols.
     **************************************************************************************/
    std::set<std::string> getTop50PairsByVolume();

    /**************************************************************************************
     * Purpose : Compute updated trackedPairs mapping for the new date.
     * Args    : prev            - Previously stored tracked pair stats.
     *           top50           - Current day's top-50 volume symbols.
     *           prev_exists     - Whether previous data exists in DB.
     *           current_date    - Date for which we compute stats.
     * Return  : TrackedData     - Newly computed tracked data struct.
     **************************************************************************************/
    TrackedData getNewTrackedPairs(
        const TrackedData& prev,
        const std::set<std::string>& top50,
        bool prev_exists,
        std::chrono::year_month_day current_date
    );

    /**************************************************************************************
     * Purpose : Fetch up to 100 days of OHLCV (1d candles) ending exactly at `targetDate`.
     *           Caller guarantees `targetDate` is the last full day (e.g., yesterday).
     *
     * Args    : targetDate   - YYYY-MM-DD date for last complete candle
     *           dataToDownload - map<pair → days to download>
     *
     * Return  : OHLCVData - Structure: result.data[pair][YYYYMMDD] = OHLCV{...}
     **************************************************************************************/
    OHLCVData fetchDataOHLCV(std::chrono::year_month_day targetDate, const std::map<std::string,int>& dataToDownload);

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
    bool storeDataOHLCV(sqlite3* db, const OHLCVData& data);

    /**************************************************************************************
     * Purpose : Prints all OHLCV rows stored for the latest date available in the 
     *           `ohlcv_data` table. This is intended for debugging and verification that 
     *           the daily fetch and storage operations are working correctly.
     *
     * Args    : db - Valid SQLite database handle.
     *
     * Return  : void
     **************************************************************************************/
    void printLatestOHLCV(sqlite3* db);


    /**************************************************************************************
     * Purpose : Prints the OHLCV for BTCUSDT for the latest stored date in the database.
     *
     * Args    : db - Valid SQLite handle.
     * Return  : void
     **************************************************************************************/
    void printLatestBTCUSDT(sqlite3* db);

    
    /**************************************************************************************
     * Purpose : For each tracked pair, determine how many days have passed since the last
     *           OHLCV candle stored in the DB.
     *
     *           Rules:
     *              - If pair has no rows → return 100
     *              - If last date is > 100 days before current date → return 100
     *              - Otherwise: return (currentYMD - lastStoredYMD)
     *
     * Args    : db          - opened SQLite database.
     *           tracked     - TrackedData (contains the map<pair → days_out>).
     *           currentYMD  - current date (year_month_day) to compare against.
     *
     * Return  : std::map<std::string,int> → map of pair → day difference.
     **************************************************************************************/
    std::map<std::string,int> computeDaysSinceLastStoredOHLCV(sqlite3* db, const TrackedData& tracked, std::chrono::year_month_day currentDate);
    
};
