#include "historical_market_data.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {

std::chrono::year_month_day parseDate(const std::string& value)
{
    if (value.size() != 10 || value[4] != '-' || value[7] != '-')
        throw std::runtime_error("Invalid CSV date: " + value);

    int year = std::stoi(value.substr(0, 4));
    unsigned month = static_cast<unsigned>(std::stoi(value.substr(5, 2)));
    unsigned day = static_cast<unsigned>(std::stoi(value.substr(8, 2)));

    std::chrono::year_month_day ymd{
        std::chrono::year{year},
        std::chrono::month{month},
        std::chrono::day{day}
    };

    if (!ymd.ok())
        throw std::runtime_error("Invalid CSV date: " + value);

    return ymd;
}

std::int64_t toUnixMillis(std::chrono::year_month_day date)
{
    auto tp = std::chrono::sys_days{date};
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        tp.time_since_epoch()).count();
}

std::string toExchangeSymbol(const std::string& sourceSymbol)
{
    if (sourceSymbol.size() >= 4 &&
        sourceSymbol.compare(sourceSymbol.size() - 4, 4, "USDT") == 0)
    {
        return sourceSymbol;
    }

    return sourceSymbol + "USDT";
}

std::string toBaseAsset(const std::string& sourceSymbol)
{
    if (sourceSymbol.size() >= 4 &&
        sourceSymbol.compare(sourceSymbol.size() - 4, 4, "USDT") == 0)
    {
        return sourceSymbol.substr(0, sourceSymbol.size() - 4);
    }

    return sourceSymbol;
}

}

HistoricalMarketData::HistoricalMarketData(
    std::filesystem::path csvPath,
    std::int64_t clockTimeMs)
    : csvPath_(std::move(csvPath)),
      clockTimeMs_(clockTimeMs)
{
    loadCsv();
}

std::int64_t HistoricalMarketData::clockTimeMs() const
{
    return clockTimeMs_.load();
}

void HistoricalMarketData::setClockTimeMs(std::int64_t clockTimeMs)
{
    clockTimeMs_.store(clockTimeMs);
}

std::vector<SimulatedInstrument> HistoricalMarketData::activeInstruments() const
{
    std::vector<SimulatedInstrument> result;

    const std::int64_t clockTimeMs = clockTimeMs_.load();
    const std::int64_t lastClosedOpenTime =
        clockTimeMs - std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::days{1}).count();

    for (const auto& [exchangeSymbol, rows] : klinesByExchangeSymbol_)
    {
        if (rows.empty() ||
            rows.front().openTimeMs > lastClosedOpenTime ||
            rows.back().openTimeMs < lastClosedOpenTime)
        {
            continue;
        }

        auto baseIt = baseAssetByExchangeSymbol_.find(exchangeSymbol);
        if (baseIt == baseAssetByExchangeSymbol_.end())
            continue;

        result.push_back({exchangeSymbol, baseIt->second});
    }

    return result;
}

std::vector<SimulatedKline> HistoricalMarketData::klines(
    const std::string& exchangeSymbol,
    std::int64_t startTimeMs,
    std::int64_t endTimeMs,
    std::size_t limit) const
{
    std::vector<SimulatedKline> result;

    auto symbolIt = klinesByExchangeSymbol_.find(exchangeSymbol);
    if (symbolIt == klinesByExchangeSymbol_.end())
        return result;

    const std::int64_t safeEndTime = std::min(endTimeMs, clockTimeMs_.load());
    if (startTimeMs >= safeEndTime || limit == 0)
        return result;

    const auto& rows = symbolIt->second;
    auto it = std::lower_bound(
        rows.begin(), rows.end(), startTimeMs,
        [](const SimulatedKline& row, std::int64_t timestamp) {
            return row.openTimeMs < timestamp;
        });

    while (it != rows.end() && it->openTimeMs < safeEndTime && result.size() < limit)
    {
        result.push_back(*it);
        ++it;
    }

    return result;
}

void HistoricalMarketData::loadCsv()
{
    std::ifstream file(csvPath_);
    if (!file.is_open())
        throw std::runtime_error("Failed to open CSV: " + csvPath_.string());

    std::string line;
    if (!std::getline(file, line))
        throw std::runtime_error("CSV is empty: " + csvPath_.string());

    std::size_t lineNumber = 1;
    while (std::getline(file, line))
    {
        ++lineNumber;
        if (line.empty())
            continue;

        std::stringstream ss(line);

        std::string dateStr;
        std::string symbol;
        std::string open;
        std::string high;
        std::string low;
        std::string close;
        std::string volume;

        std::getline(ss, dateStr, ',');
        std::getline(ss, symbol, ',');
        std::getline(ss, open, ',');
        std::getline(ss, high, ',');
        std::getline(ss, low, ',');
        std::getline(ss, close, ',');
        std::getline(ss, volume, ',');

        if (!volume.empty() && volume.back() == '\r')
            volume.pop_back();

        if (dateStr.empty() || symbol.empty() || open.empty() || high.empty() ||
            low.empty() || close.empty() || volume.empty())
        {
            throw std::runtime_error(
                "Malformed CSV row at line " + std::to_string(lineNumber));
        }

        auto date = parseDate(dateStr);
        std::int64_t openTimeMs = toUnixMillis(date);
        std::int64_t closeTimeMs = toUnixMillis(
            std::chrono::year_month_day{
                std::chrono::sys_days{date} + std::chrono::days{1}
            }) - 1;

        std::string exchangeSymbol = toExchangeSymbol(symbol);
        baseAssetByExchangeSymbol_[exchangeSymbol] = toBaseAsset(symbol);

        klinesByExchangeSymbol_[exchangeSymbol].push_back({
            openTimeMs,
            closeTimeMs,
            open,
            high,
            low,
            close,
            volume
        });
    }

    for (auto& [_, rows] : klinesByExchangeSymbol_)
    {
        std::sort(rows.begin(), rows.end(),
                  [](const SimulatedKline& a, const SimulatedKline& b) {
                      return a.openTimeMs < b.openTimeMs;
                  });
    }
}
