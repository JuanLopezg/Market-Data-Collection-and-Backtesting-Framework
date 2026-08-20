#include "database_utils.h"
#include "logger.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sqlite3.h>
#include <sstream>

#include <stdexcept>
#include <string>
#include <ctime>


/**************************************************************************************
 * Purpose : CURL write callback used to accumulate incoming HTTP response data into
 *
 *           a std::string. libcurl calls this function repeatedly while downloading
 *           content. The user-provided buffer (userp) is appended to as data arrives.
 *
 * Args    : contents - Pointer to the downloaded data chunk.
 *           size     - Size of each element (usually 1).
 *           nmemb    - Number of elements in this chunk.
 *           userp    - Pointer to the std::string accumulator.
 *
 * Return  : size_t   - Total bytes processed (size * nmemb), required by libcurl.
 **************************************************************************************/
size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp)
{
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}


/**************************************************************************************
 * Purpose : Generate a unique backtest database path using the current UTC timestamp
 *
 * This function validates that the provided path exists and is a directory, then
 * generates a filename of the form:
 *
 * backtest_YYMMDD_HHMMSS.db
 *
 * using the current UTC time. If a file with the generated name already exists,
 * an exception is thrown. The function does not create the file; it only returns
 * the resolved path.
 *
 * Args    : directory - path to an existing directory
 *
 * Return  : Full filesystem path for the new backtest database
 *
 * Throws  : std::runtime_error if the path does not exist, is not a directory,
 *           or if a file with the generated name already exists
 **************************************************************************************/
std::filesystem::path generateBacktestDbPath(
    const std::filesystem::path& directory)
{
    namespace fs = std::filesystem;

    // Validate directory
    if (!fs::exists(directory)) {
        throw std::runtime_error("Path does not exist: " + directory.string());
    }

    if (!fs::is_directory(directory)) {
        throw std::runtime_error("Path is not a directory: " + directory.string());
    }

    // Get current UTC time
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};

#if defined(_WIN32)
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif

    // Format timestamp as YYMMDD_HHMMSS (UTC)
    std::ostringstream oss;
    oss << std::put_time(&tm, "%y%m%d_%H%M%S");

    std::string filename =
        "backtest_" + oss.str() + ".db";

    fs::path fullPath = directory / filename;

    // Ensure uniqueness
    if (fs::exists(fullPath)) {
        throw std::runtime_error(
            "Backtest database already exists: " + fullPath.string()
        );
    }

    return fullPath;
}


static Timestamp parseCsvDateToTimestamp(const std::string& date)
{
    // Input:  "2020-10-05"
    // Output: 20201005
    std::string yyyymmdd;
    yyyymmdd.reserve(8);

    for (char c : date)
    {
        if (c != '-')
            yyyymmdd.push_back(c);
    }

    return static_cast<Timestamp>(std::stoul(yyyymmdd));
}


OHLCVData loadDatabaseFromCSV(
    std::filesystem::path csv_path,
    Timestamp start_date,
    Timestamp end_date)
{
    OHLCVData ohlcvData;

    std::ifstream file(csv_path);
    if (!file.is_open())
    {
        LG_ERROR("Failed to open CSV: {}", csv_path.string());
        return ohlcvData;
    }

    std::string line;

    // Skip header:
    // date,symbol,open,high,low,close,volume
    std::getline(file, line);

    while (std::getline(file, line))
    {
        if (line.empty())
            continue;

        std::stringstream ss(line);

        std::string dateStr;
        std::string symbol;
        std::string openStr;
        std::string highStr;
        std::string lowStr;
        std::string closeStr;
        std::string volumeStr;

        std::getline(ss, dateStr, ',');
        std::getline(ss, symbol, ',');
        std::getline(ss, openStr, ',');
        std::getline(ss, highStr, ',');
        std::getline(ss, lowStr, ',');
        std::getline(ss, closeStr, ',');
        std::getline(ss, volumeStr, ',');

        if (dateStr.empty() || symbol.empty())
            continue;

        Timestamp date = parseCsvDateToTimestamp(dateStr);

        if (date < start_date)
            continue;

        if (end_date != 0 && date > end_date)
            continue;

        OHLCV candle;
        candle.open   = std::stod(openStr);
        candle.high   = std::stod(highStr);
        candle.low    = std::stod(lowStr);
        candle.close  = std::stod(closeStr);
        candle.volume = std::stod(volumeStr);

        ohlcvData.data[symbol][date] = candle;
    }

    return ohlcvData;
}


OHLCVData loadDatabaseFromSQLite(
    std::filesystem::path database_path,
    Timestamp start_date,
    Timestamp end_date)
{
    OHLCVData ohlcvData;
    std::string path = database_path.string();

    // ================= OPEN DB =================
    sqlite3* db = nullptr;

    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK)
    {
        LG_ERROR("Failed to open DB: {}", sqlite3_errmsg(db));
        return ohlcvData;
    }

    // ================= PREPARE QUERY =================
    const char* sql =
        "SELECT pair, date, open, high, low, close, volume "
        "FROM ohlcv_data "
        "WHERE date >= ? "
        "AND (? = 0 OR date <= ?) "
        "ORDER BY pair, date ASC;";

    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        LG_ERROR("Prepare failed: {}", sqlite3_errmsg(db));
        sqlite3_close(db);
        return ohlcvData;
    }

    // Bind start date parameter
    sqlite3_bind_int(stmt, 1, static_cast<int>(start_date));

    // Bind end date parameter
    sqlite3_bind_int(stmt, 2, static_cast<int>(end_date));
    sqlite3_bind_int(stmt, 3, static_cast<int>(end_date));

    // ================= READ ROWS =================
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const char* pair_c =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));

        if (!pair_c) continue;

        std::string pair(pair_c);

        unsigned int date =
            static_cast<unsigned int>(sqlite3_column_int(stmt, 1));

        OHLCV candle;
        candle.open   = sqlite3_column_double(stmt, 2);
        candle.high   = sqlite3_column_double(stmt, 3);
        candle.low    = sqlite3_column_double(stmt, 4);
        candle.close  = sqlite3_column_double(stmt, 5);
        candle.volume = sqlite3_column_double(stmt, 6);

        ohlcvData.data[pair][date] = candle;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    return ohlcvData;
}


/**************************************************************************************
 * Purpose : Load OHLCV market data from a SQLite database into an OHLCVData structure
 *
 * This function opens the SQLite database located at the provided filesystem path
 * and executes a query against the `ohlcv_data` table. All rows with
 *
 * date >= start_date and date <= end_date
 *
 * are retrieved and ordered by pair and date in ascending order.
 *
 * end_date = 0 disables the upper date limit.
 *
 * Args    : database_path - filesystem path to the SQLite database file
 *           start_date    - minimum date to load (inclusive), format YYYYMMDD
 *           end_date      - maximum date to load (inclusive), format YYYYMMDD
 *
 * Return  : OHLCVData populated with all matching OHLCV rows from the database;
 *           returns an empty OHLCVData object on failure
 **************************************************************************************/
OHLCVData loadDatabase(
    std::filesystem::path database_path,
    Timestamp start_date,
    Timestamp end_date)
{
    std::string extension = database_path.extension().string();

    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); }
    );

    if (extension == ".csv")
    {
        return loadDatabaseFromCSV(database_path, start_date, end_date);
    }

    if (extension == ".db")
    {
        return loadDatabaseFromSQLite(database_path, start_date, end_date);
    }

    LG_ERROR("Unsupported database extension: {}", extension);

    return OHLCVData{};
}
