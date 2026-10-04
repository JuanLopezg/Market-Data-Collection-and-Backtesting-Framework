/*
 * File purpose: Defines the in-memory historical candle store and simulated clock used by the Binance simulator.
 *
 * Keep this file focused on this responsibility. Trading decisions belong in
 * their domain component; process orchestration belongs in the service application.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

// One Binance-style daily candle stored as wire-friendly text values.
struct SimulatedKline {
    std::int64_t openTimeMs = 0;
    std::int64_t closeTimeMs = 0;
    std::string open;
    std::string high;
    std::string low;
    std::string close;
    std::string volume;
};

// Maps the simulator exchange symbol to its base asset.
struct SimulatedInstrument {
    std::string exchangeSymbol;
    std::string baseAsset;
};

// Owns historical candles and hides any candle that is not visible at the simulated clock.
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
