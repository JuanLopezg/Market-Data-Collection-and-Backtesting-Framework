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

// Returns the current local time as a formatted string with millisecond precision.
// Args    : None
std::string nowString();

// Returns the current UTC time with millisecond precision.
// Args    : None
std::string currentUtcTimestamp();
std::string currentUtcTimestamp(std::chrono::system_clock::time_point now);

// Computes the remaining time until the next UTC midnight (00:00:00).
// Args    : None
std::string timeUntilUtcMidnight();
std::string timeUntilUtcMidnight(std::chrono::system_clock::time_point now);

// Retrieves the current UTC calendar date (year, month, day).
// Args    : None
std::chrono::year_month_day getCurrentUtcDate();
std::chrono::year_month_day getCurrentUtcDate(std::chrono::system_clock::time_point now);

// Computes the date of the previous day relative to the input date.
// Args    : ymd - A chrono::year_month_day representing the current date.
std::chrono::year_month_day getPreviousDayDate(std::chrono::year_month_day ymd);

// Formats a chrono::year_month_day into a "YYYY-MM-DD" string.
// Args    : ymd - Date to format.
std::string formatYMD(std::chrono::year_month_day ymd);

// Computes the next UTC midnight (00:00:00 of the following day).
// Args    : None
std::chrono::system_clock::time_point computeNextMidnightUTC();
std::chrono::system_clock::time_point computeNextMidnightUTC(std::chrono::system_clock::time_point now);

// Converts a chrono::year_month_day into an integer of the form YYYYMMDD.
// Args    : ymd - The date to convert.
int toYYYYMMDD(std::chrono::year_month_day ymd);

// Converts an integer date (YYYYMMDD) to a Unix timestamp in milliseconds.
// Args    : yyyymmdd - The encoded date (YYYYMMDD).
long toUnixMillis(int yyyymmdd);

// Given a compact date (YYYYMMDD), computes the previous calendar day.
// Args    : yyyymmdd - Date encoded as YYYYMMDD (e.g., 20240118).
unsigned int previousDay(unsigned int yyyymmdd);

// Given a compact date (YYYYMMDD), computes the next calendar day.
// Args    : yyyymmdd - Date encoded as YYYYMMDD (e.g., 20240118).
unsigned int nextDay(unsigned int yyyymmdd);

