#include "backtester.h"
#include "backtest_metrics.h"
#include "database_utils.h"
#include "research_strategy_definitions.h"
#include "logger.h"

#include <boost/program_options.hpp>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <map>

int main(int argc, char** argv)
try
{
    namespace options = boost::program_options;
    std::string databasePath, selectedStrategy;
    Timestamp startDate, endDate;
    double feeTaker;
    options::options_description arguments("Isolated CURRENT strategy statistics");
    arguments.add_options()
        ("help,h", "Show study options")
        ("database", options::value(&databasePath)->default_value("storage/databases/1d_cmc.csv"), "Daily CSV/SQLite input")
        ("start", options::value(&startDate)->default_value(20200101), "Cold start YYYYMMDD, inclusive")
        ("end", options::value(&endDate)->default_value(20200416), "Cutoff YYYYMMDD, inclusive")
        ("strategy", options::value(&selectedStrategy)->default_value("all"), "Strategy name or all")
        ("fee-taker", options::value(&feeTaker)->default_value(0.0), "Commission fraction per fill");
    options::variables_map values;
    options::store(options::parse_command_line(argc, argv, arguments), values);
    if (values.count("help")) {
        std::cout << arguments << '\n';
        return 0;
    }
    options::notify(values);
    const auto validDate = [](Timestamp date) {
        const std::chrono::year_month_day calendar{std::chrono::year(static_cast<int>(date / 10000)),
            std::chrono::month((date / 100) % 100), std::chrono::day(date % 100)};
        return date >= 19000101 && date <= 99991231 && calendar.ok();
    };
    if (!validDate(startDate) || !validDate(endDate) || startDate > endDate)
        throw std::invalid_argument("Require valid start <= end dates");
    if (!std::isfinite(feeTaker) || feeTaker < 0.0)
        throw std::invalid_argument("Fee must be finite and non-negative");
    auto definitions = research_studies::makeStrategyDefinitions();
    if (selectedStrategy != "all") {
        std::erase_if(definitions, [&](const auto& definition) { return definition.name != selectedStrategy; });
        if (definitions.empty())
            throw std::invalid_argument("Unknown strategy: " + selectedStrategy);
    }
    if (!std::filesystem::is_regular_file(databasePath))
        throw std::invalid_argument("Input database does not exist: " + databasePath);
    Logger::Instance().Setup(false, false, "", "", true);
    const OHLCVData data = loadDatabase(databasePath, startDate, endDate);
    if (data.data.empty())
        throw std::runtime_error("No market data in the selected window");
    constexpr double initialBalance = 100000.0;
    const BacktestMetricsSettings settings{.periodsPerYear = 365.0, .excludeSimulatedTrades = true,
        .databaseTimeframe = "1d", .annualizationBenchmarkSymbol = "BTC"};
    // Preserve this consumer's contract: metrics are logged and cached in memory.
    std::map<std::string, BacktestMetrics> metricsCache;
    for (const auto& definition : definitions) {
        BacktestContext context(data,
            research_studies::makeStudyPortfolio(definition, definition.currentParameters),
            initialBalance, feeTaker);
        Backtester tester(context);
        tester.loop();
        tester.closeTrades();
        auto metrics = calculateBacktestMetrics(definition.name, context.GetTradesHistory(),
            context.GetBalanceEquityHistoric(), context.GetMarketData(), initialBalance, settings);
        logBacktestMetrics(metrics);
        metricsCache.insert_or_assign(definition.name, std::move(metrics));
    }
    LG_INFO("Completed {} isolated strategy backtests. Metrics remain in memory only.", metricsCache.size());
    for (const auto& [name, metrics] : metricsCache) {
        LG_INFO("Summary {}: return={:.2f}% annualized={:.2f}% max_dd={:.2f}% sharpe={:.3f} "
                "sortino={:.3f} calmar={:.3f} profit_factor={:.3f} trades={}",
            name, metrics.netReturnPercent, metrics.annualizedReturnPercent, metrics.maxDrawdownPercent,
            metrics.sharpeRatio, metrics.sortinoRatio, metrics.calmarRatio, metrics.profitFactor, metrics.tradeCount);
    }
    return 0;
}
catch (const std::exception& error)
{
    std::cerr << "Strategy statistics failed: " << error.what() << '\n';
    return 1;
}
