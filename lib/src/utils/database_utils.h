#pragma once

#include <cstddef>
#include <filesystem>

#include "data_types.h"


// Generate a unique backtest database path using the current UTC timestamp
//
// This function validates that the provided path exists and is a directory, then
// generates a filename of the form:
//
// backtest_YYMMDD_HHMMSS.db
//
// using the current UTC time. If a file with the generated name already exists,
// an exception is thrown. The function does not create the file; it only returns
// the resolved path.
//
// Args    : directory - path to an existing directory
//
//
// Throws  : std::runtime_error if the path does not exist, is not a directory,
// or if a file with the generated name already exists
std::filesystem::path generateBacktestDbPath(const std::filesystem::path& directory);


// Load OHLCV market data from a SQLite database into an OHLCVData structure
//
// This function opens the SQLite database located at the provided filesystem path
// and executes a query against the `ohlcv_data` table. All rows with
//
// date >= start_date and date <= end_date
//
// are retrieved and ordered by pair and date in ascending order.
//
// end_date = 0 disables the upper date limit.
//
// Args    : database_path - filesystem path to the SQLite database file
// start_date    - minimum date to load (inclusive), format YYYYMMDD
// end_date      - maximum date to load (inclusive), format YYYYMMDD
//
// returns an empty OHLCVData object on failure
OHLCVData loadDatabase(
    std::filesystem::path database_path,
    Timestamp start_date,
    Timestamp end_date = 0
);