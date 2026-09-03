#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

struct SimulatedKline {
    std::int64_t openTimeMs = 0;
    std::int64_t closeTimeMs = 0;
    std::string open;
    std::string high;
    std::string low;
    std::string close;
    std::string volume;
};

struct SimulatedInstrument {
    std::string exchangeSymbol;
    std::string baseAsset;
};

class HistoricalMarketData {
public:
    HistoricalMarketData(std::filesystem::path csvPath, std::int64_t clockTimeMs);

    std::int64_t clockTimeMs() const;
    void setClockTimeMs(std::int64_t clockTimeMs);
    std::vector<SimulatedInstrument> activeInstruments() const;
    std::vector<SimulatedKline> klines(
        const std::string& exchangeSymbol,
        std::int64_t startTimeMs,
        std::int64_t endTimeMs,
        std::size_t limit) const;

private:
    std::filesystem::path csvPath_;
    std::atomic<std::int64_t> clockTimeMs_{0};
    std::map<std::string, std::string> baseAssetByExchangeSymbol_;
    std::map<std::string, std::vector<SimulatedKline>> klinesByExchangeSymbol_;

    void loadCsv();
};
