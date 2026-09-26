#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "historical_csv_source.h"

namespace fs = std::filesystem;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

fs::path writeFixture(const fs::path& dir, const std::string& name, const std::string& body)
{
    const fs::path path = dir / name;
    std::ofstream out(path);
    if (!out)
        throw std::runtime_error("cannot create fixture");
    out << body;
    return path;
}

int main()
{
    const fs::path dir = fs::temp_directory_path() / "algotrading_t15_source_test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    const std::string header = "date,symbol,open,high,low,close,volume\n";
    const fs::path good = writeFixture(
        dir,
        "good.csv",
        header +
        "2020-01-01,BTC,10,12,9,11,100\n"
        "2020-01-01,ETH,5,6,4,5.5,200\n"
        "2020-01-02,BTC,11,13,10,12,300\n"
        "2020-01-02,ETH,5.5,7,5,6,50\n"
        "2020-01-03,BTC,12,14,11,13,400\n");

    {
        HistoricalCsvSource source(good);
        require(source.nextDate() == Timestamp{20200101}, "nextDate must start at first source day");
        const HistoricalMarketDay day1 = source.readNextDay();
        require(day1.date == 20200101, "first day date mismatch");
        require(day1.rowCount() == 2, "first day row count mismatch");
        require(day1.volume_ranking.size() == 2, "first day ranking count mismatch");
        require(day1.volume_ranking.front().first == "ETH", "ranking must use descending source volume");
        require(source.nextDate() == Timestamp{20200102}, "cursor must stop before next day");

        const auto checkpoint = source.skipThrough(20200102);
        require(checkpoint.has_value() && checkpoint->date == 20200102, "skipThrough checkpoint mismatch");
        require(source.nextDate() == Timestamp{20200103}, "skipThrough must leave cursor after checkpoint");
    }

    {
        const fs::path duplicate = writeFixture(
            dir,
            "duplicate.csv",
            header +
            "2020-01-01,BTC,10,12,9,11,100\n"
            "2020-01-01,BTC,10,12,9,11,100\n");
        bool rejected = false;
        try {
            HistoricalCsvSource source(duplicate);
            (void)source.readNextDay();
        }
        catch (const std::exception&) {
            rejected = true;
        }
        require(rejected, "duplicate symbol/date must be rejected");
    }

    {
        const fs::path backwards = writeFixture(
            dir,
            "backwards.csv",
            header +
            "2020-01-02,BTC,10,12,9,11,100\n"
            "2020-01-01,ETH,5,6,4,5.5,200\n");
        bool rejected = false;
        try {
            HistoricalCsvSource source(backwards);
            (void)source.readNextDay();
            (void)source.nextDate();
        }
        catch (const std::exception&) {
            rejected = true;
        }
        require(rejected, "backwards source dates must be rejected");
    }

    {
        const fs::path invalidBar = writeFixture(
            dir,
            "invalid.csv",
            header +
            "2020-01-01,BTC,10,8,9,11,100\n");
        bool rejected = false;
        try {
            HistoricalCsvSource source(invalidBar);
            (void)source.readNextDay();
        }
        catch (const std::exception&) {
            rejected = true;
        }
        require(rejected, "invalid OHLCV must be rejected");
    }

    fs::remove_all(dir);
    std::cout << "PASS: T15 historical CSV source unit tests\n";
    return 0;
}
