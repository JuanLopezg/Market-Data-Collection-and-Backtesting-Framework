#pragma once

#include <string>
#include <filesystem>

/**************************************************************************************
 * Purpose : Declaration of the CURL write callback used when downloading data via
 *           libcurl. The callback appends incoming data chunks into a std::string
 *           provided by the caller.
 *
 * Args    : contents - Pointer to the memory block containing downloaded data.
 *           size     - Size of each element in the block (usually 1).
 *           nmemb    - Number of elements in the block.
 *           userp    - Pointer to a std::string where data should be appended.
 *
 * Return  : size_t   - Total bytes processed (size * nmemb). libcurl requires the
 *                      callback to return the number of bytes handled.
 **************************************************************************************/
size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp);


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
std::filesystem::path generateBacktestDbPath(const std::filesystem::path& directory);