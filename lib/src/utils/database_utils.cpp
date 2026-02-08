#include "database_utils.h"

#include <string>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>

/**************************************************************************************
 * Purpose : CURL write callback used to accumulate incoming HTTP response data into
 *           a std::string. libcurl calls this function repeatedly while downloading
 *           content. The user-provided buffer (userp) is appended to as data arrives.
 * Args    : contents - Pointer to the downloaded data chunk.
 *           size     - Size of each element (usually 1).
 *           nmemb    - Number of elements in this chunk.
 *           userp    - Pointer to the std::string accumulator.
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
 *   backtest_YYMMDD_HHMMSS.db
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

