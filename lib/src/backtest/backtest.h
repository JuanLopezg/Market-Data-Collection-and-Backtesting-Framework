#pragma once

#include <cstddef>
#include <filesystem>

#include <sqlite3.h>

#include "backtest_context.h"
#include "simulated_exchange.h"
#include "trading_engine.h"


/**************************************************************************************
 * Type    : Backtester
 * Purpose : Historical clock/runtime around the shared TradingEngine
 *
 * Timing convention:
 *   Day T close  -> TradingEngine::onBarClose()
 *   Day T+1 open -> TradingEngine::executePendingPlans() -> simulated exchange events
 **************************************************************************************/
class Backtester {
public:
    explicit Backtester(BacktestContext& backtestContext);

    void loop();

    void storeResults(std::filesystem::path& backtestStorePath);
    void storeTradesCSV(const std::filesystem::path& csvStorePath);
    void storeVolatilityDiagnosticsCSV(
        const std::filesystem::path& csvStorePath,
        std::size_t rollingWindow,
        double periodsPerYear
    );

    // Kept for caller compatibility. Open trades are reported mark-to-market and are not
    // force-filled/closed merely because historical data ended.
    void closeTrades() {}

private:
    BacktestContext& backtest_context_;
    SimulatedExchange simulated_exchange_;
    TradingEngine trading_engine_;

    Timestamp starting_date_ = 0;

    void executePendingPlansAtOpen(Timestamp ts, const CoinBarMap& bars);
    void calculateClosePlans(const MarketData& marketData, Timestamp ts, const PriceSnapshot& closePrices);

    static PriceSnapshot buildOpenPrices(const CoinBarMap& bars);
    static PriceSnapshot buildClosePrices(const CoinBarMap& bars);

    void storeTrades(sqlite3* db);
    void storeBalanceEquity(sqlite3* db);
};
