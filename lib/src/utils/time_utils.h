#pragma once

#include <chrono>
#include <ctime>
#include <fmt/format.h>
#include <fmt/chrono.h>
#include <fstream>
#include <stdexcept>
#include <nlohmann/json-schema.hpp>
#include <string>
#include <nlohmann/json.hpp>

/**************************************************************************************
 * Purpose : Returns the current local time as a formatted string with millisecond precision.
 * Args    : None
 * Return  : std::string - Local timestamp in the format "YYYY-MM-DD HH:MM:SS.mmm"
 **************************************************************************************/
std::string nowString();

/**************************************************************************************
 * Purpose : Returns the current UTC time with millisecond precision.
 * Args    : None
 * Return  : std::string - UTC time formatted as "HH:MM:SS.mmm UTC"
 **************************************************************************************/
std::string currentUtcTimestamp();

/**************************************************************************************
 * Purpose : Computes the remaining time until the next UTC midnight (00:00:00).
 * Args    : None
 * Return  : std::string - Formatted duration "HHh MMm SSs until UTC midnight"
 **************************************************************************************/
std::string timeUntilUtcMidnight();

/**************************************************************************************
 * Purpose : Retrieves the current UTC calendar date (year, month, day).
 * Args    : None
 * Return  : std::chrono::year_month_day - Current date in UTC.
 **************************************************************************************/
std::chrono::year_month_day getCurrentUtcDate();

/**************************************************************************************
 * Purpose : Computes the date of the previous day relative to the input date.
 * Args    : ymd - A chrono::year_month_day representing the current date.
 * Return  : std::chrono::year_month_day - The previous day's date.
 **************************************************************************************/
std::chrono::year_month_day getPreviousDayDate(std::chrono::year_month_day ymd);

/**************************************************************************************
 * Purpose : Formats a chrono::year_month_day into a "YYYY-MM-DD" string.
 * Args    : ymd - Date to format.
 * Return  : std::string - Formatted date string.
 **************************************************************************************/
std::string formatYMD(std::chrono::year_month_day ymd);

/**************************************************************************************
 * Purpose : Computes the next UTC midnight (00:00:00 of the following day).
 * Args    : None
 * Return  : std::chrono::system_clock::time_point - Timestamp of next midnight UTC.
 **************************************************************************************/
std::chrono::system_clock::time_point computeNextMidnightUTC();

/**************************************************************************************
 * Purpose : Converts a chrono::year_month_day into an integer of the form YYYYMMDD.
 * Args    : ymd - The date to convert.
 * Return  : int - The compact date representation (e.g., 20240118).
 **************************************************************************************/
int toYYYYMMDD(std::chrono::year_month_day ymd);

/**************************************************************************************
 * Purpose : Converts an integer date (YYYYMMDD) to a Unix timestamp in milliseconds.
 * Args    : yyyymmdd - The encoded date (YYYYMMDD).
 * Return  : long - Unix timestamp (ms since 1970-01-01 00:00:00 UTC).
 **************************************************************************************/
long toUnixMillis(int yyyymmdd);
