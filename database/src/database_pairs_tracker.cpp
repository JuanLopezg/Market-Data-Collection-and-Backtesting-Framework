#include "database_downloader.h"
#include "logger.h"
#include <fmt/chrono.h>
#include <sqlite3.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <string>
#include <ctime>
#include <fmt/core.h>

using json = nlohmann::json;

/**************************************************************************************
 * Purpose : Reads the single row in tracked_pairs, reconstructing the TrackedData
 *           struct containing date and symbol → days mappings.
 * Args    : db - Valid SQLite handle.
 * Return  : TrackedData - If no row exists, date == EMPTY_DATE.
 **************************************************************************************/
TrackedData DatabaseDownloader::getTrackedPairs(sqlite3* db)
{
    TrackedData result;
    result.date = EMPTY_DATE;

    const char* sql = "SELECT date, json FROM tracked_pairs LIMIT 1;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        LG_ERROR("SQLite prepare failed: {}", sqlite3_errmsg(db));
        return result;
    }

    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const char* date_text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* json_text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));

        if (date_text && json_text)
        {
            std::string ds(date_text);

            int y = std::stoi(ds.substr(0, 4));
            unsigned m = static_cast<unsigned>(std::stoi(ds.substr(5, 2)));
            unsigned d = static_cast<unsigned>(std::stoi(ds.substr(8, 2)));

            std::chrono::year year_obj{y};
            std::chrono::month month_obj{m};
            std::chrono::day day_obj{d};

            if (year_obj.ok() && month_obj.ok() && day_obj.ok())
                result.date = year_obj / month_obj / day_obj;
            else
                result.date = EMPTY_DATE;

            json j = json::parse(json_text);

            for (auto it = j.begin(); it != j.end(); ++it)
            {
                const std::string& pair = it.key();
                unsigned int days = it.value().get<unsigned int>();
                result.trackedPairs[pair] = days;
            }
        }
    }

    sqlite3_finalize(stmt);
    return result;
}

/**************************************************************************************
 * Purpose : Computes the new tracked pair state for the given date based on:
 *              - previous tracked data
 *              - current top-50 pairs
 *              - whether previous data existed
 * Args    : prev         - Previous day's tracked data.
 *           top50        - Current top-50 pair set.
 *           prev_exists  - Whether previous data exists.
 *           date         - Current date for which data is computed.
 * Return  : TrackedData  - New tracked data.
 **************************************************************************************/
TrackedData DatabaseDownloader::getNewTrackedPairs(
        const TrackedData& prev,
        const std::set<std::string>& top50,
        bool prev_exists,
        std::chrono::year_month_day date)
{
    TrackedData result;
    result.date = date;

    // No previous data → initialize all top50 pairs with 0 days
    if (!prev_exists)
    {
        for (const auto& p : top50)
            result.trackedPairs[p] = 0;
        return result;
    }

    // Compute day difference
    int diff = (std::chrono::sys_days(date) - std::chrono::sys_days(prev.date)).count();
    if (diff < 1) {diff = 1; LG_ERROR("wtf diff <1?");}

    // Coins in top 50 → reset to zero
    for (const auto& p : top50)
        result.trackedPairs[p] = 0;

    // Coins not in top 50 → increment days
    for (const auto& [oldPair, oldDays] : prev.trackedPairs)
    {
        if (!top50.contains(oldPair))
            result.trackedPairs[oldPair] = oldDays + diff;
    }

    return result;
}
