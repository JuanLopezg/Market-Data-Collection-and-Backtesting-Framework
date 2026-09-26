#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include "historical_csv_source.h"
#include "time_handler.h"

namespace {

Timestamp newestCompleted(const TimeHandler& handler)
{
    using namespace std::chrono;
    const auto today = floor<days>(handler.getTime());
    const year_month_day ymd{sys_days{today - days{1}}};
    const int value = int(ymd.year()) * 10000 +
        static_cast<int>(unsigned(ymd.month())) * 100 +
        static_cast<int>(unsigned(ymd.day()));
    return static_cast<Timestamp>(value);
}

TimeHandler handlerAt(const std::chrono::system_clock::time_point simulatedNow)
{
    const auto realReference = std::chrono::system_clock::now();
    const auto bias = simulatedNow - realReference;
    return TimeHandler{1.0, bias, realReference};
}

} // namespace

int main()
{
    using namespace std::chrono;
    const auto path = std::filesystem::temp_directory_path() / "algotrading_t16_visibility.csv";
    {
        std::ofstream out(path);
        out << "date,symbol,open,high,low,close,volume\n";
        out << "2020-01-01,BTC,10,12,9,11,100\n";
        out << "2020-01-01,ETH,20,22,19,21,200\n";
        out << "2020-01-02,BTC,99,101,98,100,999\n";
        out << "2020-01-02,ETH,199,201,198,200,1999\n";
    }

    try {
        // At noon on Jan 2, Jan 1 is the newest fully completed daily candle.
        const sys_days jan2 = year{2020}/1/2;
        TimeHandler handler = handlerAt(jan2 + hours{12});
        const Timestamp visible = newestCompleted(handler);
        if (visible != 20200101)
            throw std::runtime_error("unexpected completed-day boundary");

        HistoricalCsvSource source(path);
        const auto first = source.nextDate();
        if (!first || *first != 20200101)
            throw std::runtime_error("first historical date mismatch");
        if (*first > visible)
            throw std::runtime_error("visible source date unexpectedly in future");

        const HistoricalMarketDay day1 = source.readNextDay();
        if (day1.date != 20200101 || day1.rowCount() != 2)
            throw std::runtime_error("visible day read mismatch");

        // nextDate()/peekDateOnly may inspect only the date field to decide visibility;
        // it must not consume or parse Jan-2 OHLCV while Jan-2 is not yet completed.
        const auto future = source.nextDate();
        if (!future || *future != 20200102 || *future <= visible)
            throw std::runtime_error("future date boundary mismatch");

        // Once business time advances to Jan 3, Jan 2 becomes visible and is still
        // available intact, proving the earlier visibility check did not consume it.
        TimeHandler nextHandler = handlerAt(sys_days{year{2020}/1/3} + minutes{1});
        if (newestCompleted(nextHandler) != 20200102)
            throw std::runtime_error("next completed-day boundary mismatch");
        const HistoricalMarketDay day2 = source.readNextDay();
        if (day2.date != 20200102 || day2.rowCount() != 2)
            throw std::runtime_error("future day was consumed/corrupted before visibility");
        if (day2.bars.data.at("BTC").at(20200102).close != 100.0)
            throw std::runtime_error("future OHLCV changed while hidden");

        std::filesystem::remove(path);
        std::cout << "PASS: T16 historical feeder visibility no-lookahead unit tests\n";
        return 0;
    }
    catch (...) {
        std::filesystem::remove(path);
        throw;
    }
}
