#include "backtester.h"
#include "database_utils.h"
#include "logger.h"
#include "parameter_sweep.h"
#include "research_strategy_definitions.h"
#include "sensitivity_study.h"

#include <boost/program_options.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace research_studies;

// Unlike the HTML, BTC and XH studies, this study skips combinations rejected by
// isValidCombination instead of recording them, and its <name>_sensitivity.csv
// holds only run_id, return, drawdown, trade count and the parameter values.
template <typename Callback>
void forEachValidCombination(
    const StrategyDefinition& definition,
    const std::vector<ActiveSensitivityParameter>& active,
    Callback&& callback
)
{
    ParameterValues parameters = definition.currentParameters;
    auto validOnly = [&](const ParameterValues& combination) {
        if (!definition.isValidCombination || definition.isValidCombination(combination)) {
            callback(combination);
        }
    };
    enumerateSensitivityCombinations(active, 0U, parameters, validOnly);
}

std::size_t countValidCombinations(const StrategyDefinition& definition, bool keepEndpointsOnly)
{
    const std::vector<ActiveSensitivityParameter> active =
        activeSensitivityParameters(definition, keepEndpointsOnly);
    if (active.empty()) {
        return 0U;
    }

    std::size_t count = 0U;
    forEachValidCombination(definition, active, [&count](const ParameterValues&) { ++count; });
    return count;
}

std::vector<ParameterSensitivityReport> runInitialSensitivity(
    const StrategyDefinition& definition,
    const OHLCVData& ohlcvData,
    const StudyRunSettings& settings,
    const std::filesystem::path& resultsPath,
    SensitivityProgressReporter& progress
)
{
    const std::vector<ActiveSensitivityParameter> active =
        activeSensitivityParameters(definition, settings.keepEndpointsOnly);

    if (active.empty()) {
        return {};
    }

    // Parameter sensitivity needs only return, drawdown, and trade count.
    // Disabling Monte Carlo here avoids running 10,000 simulations for every
    // grid point. The normal report still uses the configured Monte Carlo run.
    BacktestMetricsSettings sensitivityMetricsSettings = settings.metricsSettings;
    sensitivityMetricsSettings.monteCarloSimulationCount = 0U;
    sensitivityMetricsSettings.monteCarloFanPointCount = 0U;

    SensitivityAggregator aggregator(active);
    progress.setStrategy(definition.name);
    std::ofstream results(resultsPath);
    results.exceptions(std::ios::failbit | std::ios::badbit);
    results << "run_id,net_return_percent,max_drawdown_percent,trade_count";
    for (const auto& [key, value] : definition.currentParameters) {
        (void)value;
        results << ',' << key;
    }
    results << '\n' << std::setprecision(17);
    std::size_t runId = 0;

    forEachValidCombination(definition, active, [&](const ParameterValues& combination) {
        const BacktestMetrics metrics = runSingleBacktest(
            definition,
            combination,
            ohlcvData,
            settings,
            sensitivityMetricsSettings
        );
        results << ++runId << ',' << metrics.netReturnPercent << ','
                << metrics.maxDrawdownPercent << ',' << metrics.tradeCount;
        for (const auto& [key, value] : combination) {
            (void)key;
            results << ',' << value;
        }
        results << '\n';

        aggregator.add(combination, metrics);
        progress.advance();
    });

    return aggregator.reports();
}

// Baseline run with <name>_trades.csv and <name>_account.csv, the optional grid,
// then the HTML report.
BacktestMetrics runInitialStrategyReport(
    const StrategyDefinition& definition,
    const OHLCVData& ohlcvData,
    const StudyRunSettings& settings,
    const std::filesystem::path& reportPath,
    SensitivityProgressReporter* progress
)
{
    BacktestContext context(
        ohlcvData,
        settings.makePortfolio(definition, definition.currentParameters),
        settings.initialBalance,
        settings.feeTaker
    );
    Backtester tester(context);
    tester.loop();

    tester.storeTradesCSV(reportPath.parent_path() / (definition.name + "_trades.csv"));
    std::ofstream accountOutput(reportPath.parent_path() / (definition.name + "_account.csv"));
    accountOutput.exceptions(std::ios::failbit | std::ios::badbit);
    accountOutput << "timestamp,cash,balance,equity\n" << std::setprecision(17);
    for (const auto& snapshot : context.GetAccountHistory())
        accountOutput << snapshot.timestamp << ',' << snapshot.cash << ',' << snapshot.balance << ',' << snapshot.equity << '\n';

    BacktestMetrics metrics = calculateBacktestMetrics(
        definition.name,
        context.GetTradesHistory(),
        context.GetBalanceEquityHistoric(),
        context.GetMarketData(),
        settings.initialBalance,
        settings.metricsSettings
    );

    std::vector<ParameterSensitivityReport> parameterSensitivity;
    if (progress != nullptr) {
        parameterSensitivity = runInitialSensitivity(
            definition,
            ohlcvData,
            settings,
            reportPath.parent_path() / (definition.name + "_sensitivity.csv"),
            *progress
        );
    }

    if (!writeBacktestHtmlReport(
        reportPath,
        metrics,
        context.GetBalanceEquityHistoric(),
        context.GetMarketData(),
        parameterSensitivity
    ))
        throw std::runtime_error("Could not write report for " + definition.name);

    return metrics;
}

} // namespace

int main(int argc, char** argv)
try
{
    namespace options = boost::program_options;
    std::string databasePath, outputRoot, studyId, selectedStrategy, grid;
    Timestamp startDate, endDate;
    double feeTaker;
    options::options_description arguments("Initial strategy parameter study");
    arguments.add_options()
        ("help,h", "Show study options")
        ("database", options::value(&databasePath)->default_value("storage/databases/1d_cmc.csv"), "Daily CSV/SQLite input")
        ("start", options::value(&startDate)->default_value(20200101), "Cold start YYYYMMDD, inclusive")
        ("end", options::value(&endDate)->default_value(20200416), "Cutoff YYYYMMDD, inclusive")
        ("output-root", options::value(&outputRoot)->default_value("storage/backtests/strategy_reports"), "Study parent directory")
        ("study-id", options::value(&studyId)->default_value(""), "New directory name; existing studies are rejected")
        ("strategy", options::value(&selectedStrategy)->default_value("all"), "Strategy name or all")
        ("grid", options::value(&grid)->default_value("none"), "none, compact endpoints, or original full grid")
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
    if (grid != "none" && grid != "compact" && grid != "full")
        throw std::invalid_argument("Grid must be none, compact or full");
    if (!std::isfinite(feeTaker) || feeTaker < 0.0)
        throw std::invalid_argument("Fee must be finite and non-negative");
    if (studyId.empty())
        studyId = "current_initial_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    if (studyId == "." || studyId == ".." || studyId.find_first_of("/\\,\"\r\n") != std::string::npos)
        throw std::invalid_argument("Study ID must be a directory name without CSV delimiters");
    auto definitions = makeStrategyDefinitions();
    if (selectedStrategy != "all") {
        std::erase_if(definitions, [&](const auto& definition) { return definition.name != selectedStrategy; });
        if (definitions.empty())
            throw std::invalid_argument("Unknown strategy: " + selectedStrategy);
    }
    if (!std::filesystem::is_regular_file(databasePath))
        throw std::invalid_argument("Input database does not exist: " + databasePath);
    Logger::Instance().Setup(false, false); // stdout only
    const OHLCVData data = loadDatabase(databasePath, startDate, endDate);
    if (data.data.empty())
        throw std::runtime_error("No market data in the selected window");
    const auto directory = std::filesystem::path(outputRoot) / studyId;
    std::filesystem::create_directories(directory.parent_path());
    if (!std::filesystem::create_directory(directory))
        throw std::runtime_error("Study directory already exists: " + directory.string());
    const bool keepEndpointsOnly = grid == "compact";
    std::size_t runs = 0;
    if (grid != "none") {
        for (const auto& definition : definitions)
            runs += countValidCombinations(definition, keepEndpointsOnly);
    }
    std::unique_ptr<SensitivityProgressReporter> progress;
    if (runs > 0)
        progress = std::make_unique<SensitivityProgressReporter>(runs);
    BacktestMetricsSettings settings{.periodsPerYear = 365.0, .excludeSimulatedTrades = true,
        .databaseTimeframe = "1d", .annualizationBenchmarkSymbol = "BTC"};
    const StudyRunSettings runSettings{.makePortfolio = makeStudyPortfolio, .initialBalance = 100000.0,
        .feeTaker = feeTaker, .metricsSettings = settings, .keepEndpointsOnly = keepEndpointsOnly};
    std::ofstream summary(directory / "metrics.csv");
    summary.exceptions(std::ios::failbit | std::ios::badbit);
    summary << "strategy,net_return_percent,max_drawdown_percent,trade_count,final_equity\n" << std::setprecision(17);
    std::ofstream metadata(directory / "study_metadata.csv");
    metadata.exceptions(std::ios::failbit | std::ios::badbit);
    metadata << "study_id,runtime,start,end,fee_taker,grid\n" << std::setprecision(17)
             << studyId << ",CURRENT," << startDate << ',' << endDate << ',' << feeTaker << ',' << grid << '\n';
    std::ofstream ranges(directory / "parameter_grid.csv");
    ranges.exceptions(std::ios::failbit | std::ios::badbit);
    ranges << "strategy,parameter,current,minimum,maximum,spacing,enabled\n" << std::setprecision(17);
    for (const auto& definition : definitions) {
        for (const auto& parameter : definition.sensitivityParameters)
            ranges << definition.name << ',' << parameter.key << ',' << definition.currentParameters.at(parameter.key)
                   << ',' << parameter.minimum << ',' << parameter.maximum << ',' << parameter.spacing
                   << ',' << parameter.enabled << '\n';
        const auto metrics = runInitialStrategyReport(definition, data, runSettings,
            directory / (definition.name + ".html"), progress.get());
        logBacktestMetrics(metrics);
        summary << definition.name << ',' << metrics.netReturnPercent << ',' << metrics.maxDrawdownPercent
                << ',' << metrics.tradeCount << ',' << metrics.finalEquity << '\n';
    }
    if (progress)
        progress->finish();
    LG_INFO("Initial parameter study written to {}", directory.string());
    return 0;
}
catch (const std::exception& error)
{
    LG_ALERT("Initial parameter study failed: {}", error.what());
    return 1;
}
