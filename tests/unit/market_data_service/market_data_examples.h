#pragma once

#include <chrono>
#include <cmath>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include "market_store.h"

namespace market_data_test {

using Json = nlohmann::json;
using Date = std::chrono::year_month_day;
inline constexpr Date JAN1 = std::chrono::year{2024}/1/1;
inline constexpr Date JAN2 = std::chrono::year{2024}/1/2;
inline constexpr Date JAN3 = std::chrono::year{2024}/1/3;
inline constexpr Date JAN5 = std::chrono::year{2024}/1/5;
inline constexpr long long JAN1_MS = 1704067200000LL;
inline constexpr long long JAN2_MS = 1704153600000LL;
inline constexpr long long JAN3_MS = 1704240000000LL;
inline constexpr long long HOUR_MS = 3600000LL;

inline long long millis(Date date)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::sys_days{date}.time_since_epoch()).count();
}

inline OHLCV exampleBar()
{
    return OHLCV{100, 120, 90, 110, 7, 731};
}

inline OHLCVData barsFor(const std::string& symbol, std::initializer_list<Timestamp> days)
{
    OHLCVData result;
    for (Timestamp day : days)
        result.data[symbol][day] = exampleBar();
    return result;
}

// Observe the durable public SQLite schema through an independent connection. The
// helper does not reconstruct the store's SQL or use its private implementation.
class Database {
public:
    explicit Database(const std::filesystem::path& path)
    {
        sqlite3* raw = nullptr;
        const int status = sqlite3_open(path.string().c_str(), &raw);
        connection_.reset(raw);
        if (status != SQLITE_OK)
            throw std::runtime_error("Cannot open test SQLite database");
    }

    void exec(const std::string& sql)
    {
        char* error = nullptr;
        if (sqlite3_exec(connection_.get(), sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
            const std::string message = error ? error : "SQLite execution failed";
            sqlite3_free(error);
            throw std::runtime_error(message);
        }
    }

    Json rows(const std::string& sql)
    {
        sqlite3_stmt* raw = nullptr;
        const int status = sqlite3_prepare_v2(connection_.get(), sql.c_str(), -1, &raw, nullptr);
        const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
        if (status != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(connection_.get()));
        Json result = Json::array();
        int step = SQLITE_OK;
        while ((step = sqlite3_step(statement.get())) == SQLITE_ROW) {
            Json row = Json::array();
            for (int column = 0; column < sqlite3_column_count(statement.get()); ++column) {
                switch (sqlite3_column_type(statement.get(), column)) {
                case SQLITE_INTEGER:
                    row.push_back(sqlite3_column_int64(statement.get(), column));
                    break;
                case SQLITE_FLOAT:
                    row.push_back(sqlite3_column_double(statement.get(), column));
                    break;
                case SQLITE_NULL:
                    row.push_back(nullptr);
                    break;
                case SQLITE_TEXT:
                    row.push_back(reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), column)));
                    break;
                default:
                    throw std::runtime_error("Unexpected SQLite value in market-data test");
                }
            }
            result.push_back(std::move(row));
        }
        if (step != SQLITE_DONE)
            throw std::runtime_error(sqlite3_errmsg(connection_.get()));
        return result;
    }

private:
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> connection_{nullptr, sqlite3_close};
};

inline Json readRows(const std::filesystem::path& path, const std::string& sql)
{
    return Database(path).rows(sql);
}

inline Json kline(long long openTime, double open = 100, double close = 110)
{
    return Json::array({openTime, std::to_string(open), "120", "90",
                        std::to_string(close), "7", openTime + 86400000 - 1, "731"});
}

inline Json nextOpen(double open = 125)
{
    // Intentionally unusable unfinished HLCV: only open(T+1) may be read.
    return Json::array({JAN3_MS, std::to_string(open), "unfinished", nullptr,
                        "unfinished", "unfinished", JAN3_MS + 86400000 - 1, "unfinished"});
}

inline Json symbol(const std::string& name, const std::string& status = "TRADING",
                   const std::string& quote = "USDT", const std::string& contract = "PERPETUAL")
{
    return Json{{"symbol", name}, {"status", status}, {"quoteAsset", quote}, {"contractType", contract}};
}

inline Json ticker(const std::string& name, const std::string& volume,
                   long long closeTime = JAN3_MS)
{
    return Json{{"symbol", name}, {"quoteVolume", volume}, {"closeTime", closeTime}};
}

inline Json settlement(long long time, const std::string& rate, const std::string& mark = "100")
{
    return Json{{"fundingTime", time}, {"fundingRate", rate}, {"markPrice", mark}};
}

} // namespace market_data_test
