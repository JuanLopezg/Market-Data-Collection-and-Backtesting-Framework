#pragma once

#include "data_types.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

// One equity-curve point is assumed to represent one database bar.
// periodsPerYear must match the database timeframe, e.g.:
//   1d  -> 365
//   4h  -> 365 * 6
// Annualized return is calculated from the number of bars available for
// annualizationBenchmarkSymbol, not from the artificial integer date span.
struct BacktestMetricsSettings {
    double periodsPerYear = 365.0;
    bool excludeSimulatedTrades = true;

    // Used for documentation/logging and to make the study settings explicit.
    // Supported timeframe strings are controlled by the study runner.
    std::string databaseTimeframe = "1d";

    // Annualization uses the number of bars available for this symbol.
    // Examples: "BTC" for your daily database, "BTCUSDT" for Binance intraday.
    std::string annualizationBenchmarkSymbol = "BTC";

    // Version 1 Monte Carlo model:
    // - 10,000 bootstrap paths by default.
    // - closed trade PnL values are sampled with replacement;
    // - every path uses the same number of trades as the historical run;
    // - each sampled PnL is scaled as a fraction of initial equity.
    std::size_t monteCarloSimulationCount = 10000U;
    std::uint64_t monteCarloSeed = 20260621ULL;
    bool tradePnlAlreadyNetOfCommission = true;

    // Number of evenly spaced trade-sequence points kept for the simulated
    // return fan chart. Values are capped at trades-per-path + 1.
    std::size_t monteCarloFanPointCount = 101U;
};

struct MonteCarloFanPoint {
    double progressPercent = 0.0;
    double p05ReturnPercent = 0.0;
    double p25ReturnPercent = 0.0;
    double medianReturnPercent = 0.0;
    double p75ReturnPercent = 0.0;
    double p95ReturnPercent = 0.0;
};

struct BacktestMetrics {
    std::string strategyName;

    double initialEquity = 0.0;
    double finalEquity = 0.0;
    double netProfit = 0.0;
    double netReturnPercent = 0.0;
    double annualizedReturnPercent = 0.0;

    // Drawdown and historical tail-risk metrics are calculated from the equity
    // curve's one-bar returns. VaR/CVaR values are positive loss magnitudes.
    double maxDrawdownAmount = 0.0;
    double maxDrawdownPercent = 0.0;
    std::size_t maxDrawdownDurationBars = 0;
    double maxDrawdownDurationDays = 0.0;
    double timeUnderwaterPercent = 0.0;
    double historicalVaR95Percent = 0.0;
    double historicalCvar95Percent = 0.0;

    double sharpeRatio = 0.0;
    double sortinoRatio = 0.0;
    double calmarRatio = 0.0;

    std::size_t tradeCount = 0;
    std::size_t winningTradeCount = 0;
    std::size_t losingTradeCount = 0;
    std::size_t breakevenTradeCount = 0;
    double grossProfit = 0.0;
    double grossLoss = 0.0; // Stored as a positive magnitude.
    double profitFactor = 0.0;
    double expectancyPerTrade = 0.0;
    double winRatePercent = 0.0;
    double averageWin = 0.0;
    double averageLoss = 0.0; // Stored as a negative value.
    double largestLoss = 0.0; // Negative value, or zero when there are no losses.

    // Historical maximum consecutive closed-trade losses.
    std::size_t maximumConsecutiveLosses = 0;
    double worstConsecutiveLossPnl = 0.0; // Negative value.

    double averageHoldingBars = 0.0;
    double exposurePercent = 0.0;
    double grossTurnover = 0.0;
    double turnoverMultiple = 0.0;

    // Monte Carlo risk metrics. Risk of ruin is explicitly defined as
    // simulated maximum drawdown >= 50%.
    std::size_t monteCarloSimulationCount = 0;
    std::size_t monteCarloTradesPerSimulation = 0;
    double monteCarloProbabilityDrawdown20Percent = 0.0;
    double monteCarloProbabilityDrawdown30Percent = 0.0;
    double monteCarloProbabilityDrawdown50Percent = 0.0;
    double monteCarloRiskOfRuinPercent = 0.0;

    double monteCarloMedianMaxDrawdownPercent = 0.0;
    double monteCarloP95MaxDrawdownPercent = 0.0;
    double monteCarloP99MaxDrawdownPercent = 0.0;

    double monteCarloMedianMaxConsecutiveLosses = 0.0;
    double monteCarloP95MaxConsecutiveLosses = 0.0;
    double monteCarloP99MaxConsecutiveLosses = 0.0;

    // Historical closed-trade net returns. Each value is calculated as:
    // net trade PnL / abs(entry price * size) * 100.
    // These values are not Monte Carlo samples; they describe the realized
    // trade-level return distribution used by the historical strategy run.
    double historicalMeanTradeReturnPercent = 0.0;
    double historicalMedianTradeReturnPercent = 0.0;
    double historicalP05TradeReturnPercent = 0.0;
    double historicalP95TradeReturnPercent = 0.0;

    // Kept in memory so the HTML report can draw the historical trade-return
    // distribution plus Monte Carlo drawdown and fan charts.
    std::vector<double> historicalTradeReturnPercentSamples;
    std::vector<double> monteCarloMaxDrawdownPercentSamples;
    std::vector<double> monteCarloHistoricalTradeReturnPercentPath;
    std::vector<MonteCarloFanPoint> monteCarloFanReturnPercent;
};

// Calculates all metrics in memory. It does not write files.
BacktestMetrics calculateBacktestMetrics(
    const std::string& strategyName,
    const std::map<TradeID, Trade>& tradesHistory,
    const std::vector<std::pair<Balance, Equity>>& balanceEquityHistoric,
    const MarketData& marketData,
    double initialEquity,
    const BacktestMetricsSettings& settings = {}
);

// Logs a readable report. It does not write metrics to disk.
void logBacktestMetrics(const BacktestMetrics& metrics);
