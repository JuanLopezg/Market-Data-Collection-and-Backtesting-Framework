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
    return currentUtcTimestamp(std::chrono::system_clock::now());
}

std::string currentUtcTimestamp(std::chrono::system_clock::time_point now)
{
    using namespace std::chrono;

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
    return timeUntilUtcMidnight(std::chrono::system_clock::now());
}

std::string timeUntilUtcMidnight(std::chrono::system_clock::time_point now)
{
    using namespace std::chrono;

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
    return getCurrentUtcDate(std::chrono::system_clock::now());
}

std::chrono::year_month_day getCurrentUtcDate(std::chrono::system_clock::time_point now)
{
    using namespace std::chrono;

    auto today = floor<days>(now);
    return year_month_day{today};
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
    return computeNextMidnightUTC(std::chrono::system_clock::now());
}

std::chrono::system_clock::time_point computeNextMidnightUTC(std::chrono::system_clock::time_point now) {
    using namespace std::chrono;

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
 *    - Conversion is performed directly from std::chrono::sys_days, so the result is
 *      always UTC and does not depend on the host machine timezone.
 **************************************************************************************/
long toUnixMillis(int yyyymmdd)
{
    int y = yyyymmdd / 10000;
    int m = (yyyymmdd / 100) % 100;
    int d = yyyymmdd % 100;

    std::chrono::year_month_day ymd{
        std::chrono::year{y},
        std::chrono::month{static_cast<unsigned>(m)},
        std::chrono::day{static_cast<unsigned>(d)}
    };

    if (!ymd.ok()) {
        throw std::invalid_argument("Invalid YYYYMMDD date");
    }

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::sys_days{ymd}.time_since_epoch());

    return static_cast<long>(ms.count());
}


unsigned int previousDay(unsigned int yyyymmdd) {
    int year  = yyyymmdd / 10000;
    int month = (yyyymmdd / 100) % 100;
    int day   = yyyymmdd % 100;

    std::chrono::year_month_day ymd{
        std::chrono::year{year},
        std::chrono::month{static_cast<unsigned>(month)},
        std::chrono::day{static_cast<unsigned>(day)}
    };

    if (!ymd.ok()) {
        throw std::invalid_argument("Invalid YYYYMMDD date");
    }

    std::chrono::sys_days prev = std::chrono::sys_days{ymd} - std::chrono::days{1};
    std::chrono::year_month_day prevYmd{prev};

    return static_cast<unsigned int>(
        int(prevYmd.year()) * 10000 +
        unsigned(prevYmd.month()) * 100 +
        unsigned(prevYmd.day())
    );
}


unsigned int nextDay(unsigned int yyyymmdd) {
    int year  = yyyymmdd / 10000;
    int month = (yyyymmdd / 100) % 100;
    int day   = yyyymmdd % 100;

    std::chrono::year_month_day ymd{
        std::chrono::year{year},
        std::chrono::month{static_cast<unsigned>(month)},
        std::chrono::day{static_cast<unsigned>(day)}
    };

    if (!ymd.ok()) {
        throw std::invalid_argument("Invalid YYYYMMDD date");
    }

    std::chrono::sys_days next = std::chrono::sys_days{ymd} + std::chrono::days{1};
    std::chrono::year_month_day nextYmd{next};

    return static_cast<unsigned int>(
        int(nextYmd.year()) * 10000 +
        unsigned(nextYmd.month()) * 100 +
        unsigned(nextYmd.day())
    );
}
