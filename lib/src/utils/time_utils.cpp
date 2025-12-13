#include "time_utils.h"

/**************************************************************************************
 * Purpose : Returns the current local time as a formatted string with millisecond precision.
 * Args    : None
 * Return  : std::string - Local timestamp in the format "YYYY-MM-DD HH:MM:SS.mmm"
 **************************************************************************************/
std::string nowString() {
    using namespace std::chrono;

    auto now = system_clock::now();
    auto ms  = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    std::time_t t = system_clock::to_time_t(now);
    std::tm tm = *std::localtime(&t);

    return fmt::format("{:%Y-%m-%d %H:%M:%S}.{:03d}", tm, ms.count());
}


/**************************************************************************************
 * Purpose : Returns the current UTC time with millisecond precision.
 * Args    : None
 * Return  : std::string - UTC time formatted as "HH:MM:SS.mmm UTC"
 **************************************************************************************/
std::string currentUtcTimestamp()
{
    using namespace std::chrono;

    auto now = system_clock::now();
    auto ms  = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    std::time_t t = system_clock::to_time_t(now);
    std::tm utc_tm = *std::gmtime(&t);

    return fmt::format("{:%H:%M:%S}.{:03d} UTC", utc_tm, ms.count());
}


/**************************************************************************************
 * Purpose : Computes the remaining time until the next UTC midnight (00:00:00).
 * Args    : None
 * Return  : std::string - Formatted duration "HHh MMm SSs until UTC midnight"
 **************************************************************************************/
std::string timeUntilUtcMidnight()
{
    using namespace std::chrono;

    auto now = system_clock::now();
    auto todayMidnight = floor<days>(now);
    auto nextMidnight = todayMidnight + days{1};

    auto remaining = nextMidnight - now;

    auto hrs = duration_cast<hours>(remaining);
    remaining -= hrs;
    auto mins = duration_cast<minutes>(remaining);
    remaining -= mins;
    auto secs = duration_cast<seconds>(remaining);

    return fmt::format("{:02d}h {:02d}m {:02d}s until UTC midnight",
                       hrs.count(), mins.count(), secs.count());
}


/**************************************************************************************
 * Purpose : Retrieves the current UTC calendar date (year, month, day).
 * Args    : None
 * Return  : std::chrono::year_month_day - Current date in UTC.
 **************************************************************************************/
std::chrono::year_month_day getCurrentUtcDate()
{
    using namespace std::chrono;

    auto now = system_clock::now();
    std::time_t t = system_clock::to_time_t(now);
    std::tm utc_tm = *std::gmtime(&t);

    year  y{utc_tm.tm_year + 1900};
    month m{utc_tm.tm_mon + 1};
    day   d{utc_tm.tm_mday};

    return year_month_day{y, m, d};
}


/**************************************************************************************
 * Purpose : Computes the date of the previous day relative to the input date.
 * Args    : ymd - A chrono::year_month_day representing the current date.
 * Return  : std::chrono::year_month_day - The previous day's date.
 **************************************************************************************/
std::chrono::year_month_day getPreviousDayDate(std::chrono::year_month_day ymd)
{
    using namespace std::chrono;

    sys_days current = sys_days{ymd};
    sys_days previous = current - days{1};

    return year_month_day{previous};
}


/**************************************************************************************
 * Purpose : Formats a chrono::year_month_day into a "YYYY-MM-DD" string.
 * Args    : ymd - Date to format.
 * Return  : std::string - Formatted date string.
 **************************************************************************************/
std::string formatYMD(std::chrono::year_month_day ymd)
{
    int y = int(ymd.year());
    unsigned m = unsigned(ymd.month());
    unsigned d = unsigned(ymd.day());

    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon  = m - 1;
    tm.tm_mday = d;

    return fmt::format("{:%Y-%m-%d}", tm);
}


/**************************************************************************************
 * Purpose : Computes the next UTC midnight (00:00:00 of the following day).
 * Args    : None
 * Return  : std::chrono::system_clock::time_point - Timestamp of next midnight UTC.
 **************************************************************************************/
std::chrono::system_clock::time_point computeNextMidnightUTC() {
    using namespace std::chrono;

    const auto now = system_clock::now();
    const auto today = floor<days>(now);
    auto midnightNext = today + days{1};

    return midnightNext;
}

/**************************************************************************************
 * Purpose : Converts a std::chrono::year_month_day into an integer of the form
 *           YYYYMMDD. This compact representation is useful for storage, comparison,
 *           hashing, and use as keys in maps (backtesting datasets, caching, etc.).
 *
 * Args    : ymd - The chrono date to convert.
 *
 * Return  : int - The encoded date as YYYYMMDD.
 *            Example: 2024/01/18 → 20240118
 **************************************************************************************/
int toYYYYMMDD(std::chrono::year_month_day ymd)
{
    int y = int(ymd.year());
    unsigned m = unsigned(ymd.month());
    unsigned d = unsigned(ymd.day());
    return y * 10000 + m * 100 + d;
}


/**************************************************************************************
 * Purpose : Converts an integer date in the format YYYYMMDD into a Unix timestamp
 *           expressed in milliseconds since epoch (UTC). This is required for all
 *           Binance Kline API requests, which expect startTime as ms since 1970.
 *
 * Args    : yyyymmdd - Integer encoded date (YYYYMMDD).
 *
 * Return  : long - Unix timestamp in milliseconds since epoch at 00:00:00 UTC of
 *                  the given date.
 *
 * Notes   :
 *    - Uses std::mktime(), which interprets struct tm as local time. If precise UTC
 *      handling is required in the future, replace with timegm() (POSIX) or a custom
 *      conversion. For Binance OHLCV (daily candles), this is acceptable because
 *      candles align at midnight UTC.
 **************************************************************************************/
long toUnixMillis(int yyyymmdd)
{
    int y = yyyymmdd / 10000;
    int m = (yyyymmdd / 100) % 100;
    int d = yyyymmdd % 100;

    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon  = m - 1;
    tm.tm_mday = d;
    tm.tm_hour = 0;
    tm.tm_min  = 0;
    tm.tm_sec  = 0;

    return static_cast<long>(std::mktime(&tm)) * 1000L;
}
