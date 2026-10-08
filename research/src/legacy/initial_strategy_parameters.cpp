#include "backtester.h"
#include "research_strategy_definitions.h"
#include "backtest_metrics.h"
#include "backtest_html_report.h"
#include "database_utils.h"
#include "logger.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <fstream>
#include <boost/program_options.hpp>

namespace {

using namespace research_studies;

bool compactSensitivity = false;

struct ActiveSensitivityParameter {
    const SensitivityParameterDefinition* definition = nullptr;
    std::vector<double> values;
};

struct SensitivityAccumulator {
    std::vector<double> finalReturnPercentValues;
    std::vector<double> maxDrawdownPercentValues;
    std::vector<double> tradeCountValues;
    std::size_t positiveCombinationCount = 0U;
    std::size_t zeroTradeCombinationCount = 0U;

    void add(const BacktestMetrics& metrics)
    {
        finalReturnPercentValues.push_back(metrics.netReturnPercent);
        maxDrawdownPercentValues.push_back(metrics.maxDrawdownPercent);
        tradeCountValues.push_back(static_cast<double>(metrics.tradeCount));

        if (metrics.netReturnPercent > 0.0) {
            ++positiveCombinationCount;
        }
        if (metrics.tradeCount == 0U) {
            ++zeroTradeCombinationCount;
        }
    }
};

class SensitivityProgressReporter {
public:
    explicit SensitivityProgressReporter(std::size_t totalRuns)
        : totalRuns_(totalRuns), start_(std::chrono::steady_clock::now())
    {
        if (totalRuns_ > 0U) {
            std::cout << "\nParameter sensitivity: " << totalRuns_
                      << " isolated backtests planned.\n";
            render(true);
        }
    }

    void setStrategy(const std::string& strategyName)
    {
        strategyName_ = strategyName;
        render(true);
    }

    void advance()
    {
        if (completedRuns_ < totalRuns_) {
            ++completedRuns_;
        }

        const std::size_t renderInterval = std::max<std::size_t>(
            1U,
            totalRuns_ / 1000U
        );

        if (completedRuns_ == totalRuns_ ||
            completedRuns_ % renderInterval == 0U) {
            render(false);
        }
    }

    void finish()
    {
        if (totalRuns_ > 0U) {
            render(true);
            std::cout << '\n';
        }
    }

private:
    static std::string formatDuration(std::chrono::seconds duration)
    {
        const auto totalSeconds = duration.count();
        const auto hours = totalSeconds / 3600;
        const auto minutes = (totalSeconds % 3600) / 60;
        const auto seconds = totalSeconds % 60;

        std::ostringstream output;
        if (hours > 0) {
            output << hours << "h ";
        }
        if (hours > 0 || minutes > 0) {
            output << minutes << "m ";
        }
        output << seconds << "s";
        return output.str();
    }

    void render(bool force)
    {
        if (totalRuns_ == 0U) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - start_
        );

        const double ratio = static_cast<double>(completedRuns_) /
            static_cast<double>(totalRuns_);
        constexpr int barWidth = 34;
        const int filled = static_cast<int>(std::round(ratio * barWidth));

        std::chrono::seconds remaining(0);
        if (completedRuns_ > 0U && completedRuns_ < totalRuns_) {
            const double remainingSeconds =
                static_cast<double>(elapsed.count()) *
                static_cast<double>(totalRuns_ - completedRuns_) /
                static_cast<double>(completedRuns_);
            remaining = std::chrono::seconds(
                static_cast<long long>(std::max(0.0, remainingSeconds))
            );
        }

        std::cout << '\r' << '[';
        for (int index = 0; index < barWidth; ++index) {
            std::cout << (index < filled ? '=' : ' ');
        }
        std::cout << "] " << std::fixed << std::setprecision(1)
                  << (ratio * 100.0) << "% "
                  << completedRuns_ << '/' << totalRuns_
                  << " | " << strategyName_
                  << " | elapsed " << formatDuration(elapsed)
                  << " | ETA " << formatDuration(remaining)
                  << std::flush;

        (void)force;
    }

    std::size_t totalRuns_ = 0U;
    std::size_t completedRuns_ = 0U;
    std::string strategyName_ = "Preparing";
    std::chrono::steady_clock::time_point start_;
};

std::vector<double> makeSweepValues(
    const SensitivityParameterDefinition& parameter
)
{
    std::vector<double> values;

    if (!parameter.enabled ||
        !std::isfinite(parameter.minimum) ||
        !std::isfinite(parameter.maximum) ||
        !std::isfinite(parameter.spacing) ||
        parameter.spacing <= 0.0 ||
        parameter.maximum < parameter.minimum) {
        return values;
    }

    constexpr std::size_t safetyLimit = 100000U;
    constexpr double epsilon = 1e-10;

    for (std::size_t index = 0U; index < safetyLimit; ++index) {
        const double value = parameter.minimum +
            static_cast<double>(index) * parameter.spacing;

        if (value > parameter.maximum + epsilon) {
            break;
        }

        values.push_back(value);
    }

    return values;
}

std::vector<ActiveSensitivityParameter> activeSensitivityParameters(
    const StrategyDefinition& definition
)
{
    std::vector<ActiveSensitivityParameter> active;

    for (const SensitivityParameterDefinition& parameter :
         definition.sensitivityParameters) {
        if (!parameter.enabled) {
            continue;
        }

        std::vector<double> values = makeSweepValues(parameter);
        if (values.empty()) {
            LG_WARN(
                "Skipping invalid sensitivity parameter '{}.{}'",
                definition.name,
                parameter.key
            );
            continue;
        }

        if (compactSensitivity && values.size() > 2)
            values = {values.front(), values.back()};
        active.push_back(ActiveSensitivityParameter{&parameter, std::move(values)});
    }

    return active;
}

template <typename Callback>
void enumerateSensitivityCombinations(
    const StrategyDefinition& definition,
    const std::vector<ActiveSensitivityParameter>& activeParameters,
    std::size_t parameterIndex,
    ParameterValues& parameters,
    Callback&& callback
)
{
    if (parameterIndex >= activeParameters.size()) {
        if (!definition.isValidCombination || definition.isValidCombination(parameters)) {
            callback(parameters);
        }
        return;
    }

    const ActiveSensitivityParameter& active = activeParameters[parameterIndex];
    for (const double value : active.values) {
        parameters.insert_or_assign(active.definition->key, value);
        enumerateSensitivityCombinations(
            definition,
            activeParameters,
            parameterIndex + 1U,
            parameters,
            std::forward<Callback>(callback)
        );
    }
}

std::size_t countSensitivityCombinations(
    const StrategyDefinition& definition
)
{
    const std::vector<ActiveSensitivityParameter> active =
        activeSensitivityParameters(definition);

    if (active.empty()) {
        return 0U;
    }

    ParameterValues parameters = definition.currentParameters;
    std::size_t count = 0U;

    enumerateSensitivityCombinations(
        definition,
        active,
        0U,
        parameters,
        [&count](const ParameterValues&) { ++count; }
    );

    return count;
}

BacktestMetrics runSingleBacktest(
    const StrategyDefinition& definition,
    const ParameterValues& parameters,
    const OHLCVData& ohlcvData,
    double initialBalance,
    double feeMaker,
    double feeTaker,
    const BacktestMetricsSettings& metricsSettings
)
{
    (void)feeMaker;
    BacktestContext context(ohlcvData, makeStudyPortfolio(definition, parameters), initialBalance, feeTaker);
    Backtester tester(context);
    tester.loop();
    tester.closeTrades();

    return calculateBacktestMetrics(
        definition.name,
        context.GetTradesHistory(),
        context.GetBalanceEquityHistoric(),
        context.GetMarketData(),
        initialBalance,
        metricsSettings
    );
}

double median(std::vector<double> values)
{
    if (values.empty()) {
        return 0.0;
    }

    std::sort(values.begin(), values.end());
    const std::size_t middle = values.size() / 2U;

    if (values.size() % 2U == 0U) {
        return (values[middle - 1U] + values[middle]) / 2.0;
    }

    return values[middle];
}

std::vector<ParameterSensitivityReport> runParameterSensitivity(
    const StrategyDefinition& definition,
    const OHLCVData& ohlcvData,
    double initialBalance,
    double feeMaker,
    double feeTaker,
    const BacktestMetricsSettings& baseMetricsSettings,
    const std::filesystem::path& resultsPath,
    SensitivityProgressReporter& progress
)
{
    const std::vector<ActiveSensitivityParameter> active =
        activeSensitivityParameters(definition);

    if (active.empty()) {
        return {};
    }

    // Parameter sensitivity needs only return, drawdown, and trade count.
    // Disabling Monte Carlo here avoids running 10,000 simulations for every
    // grid point. The normal report still uses the configured Monte Carlo run.
    BacktestMetricsSettings sensitivityMetricsSettings = baseMetricsSettings;
    sensitivityMetricsSettings.monteCarloSimulationCount = 0U;
    sensitivityMetricsSettings.monteCarloFanPointCount = 0U;

    std::vector<std::vector<SensitivityAccumulator>> accumulators;
    accumulators.reserve(active.size());
    for (const ActiveSensitivityParameter& parameter : active) {
        accumulators.emplace_back(parameter.values.size());
    }

    std::vector<std::map<double, std::size_t>> valueIndexes;
    valueIndexes.reserve(active.size());
    for (const ActiveSensitivityParameter& parameter : active) {
        std::map<double, std::size_t> indexes;
        for (std::size_t index = 0U; index < parameter.values.size(); ++index) {
            indexes.insert_or_assign(parameter.values[index], index);
        }
        valueIndexes.push_back(std::move(indexes));
    }

    ParameterValues parameters = definition.currentParameters;
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

    enumerateSensitivityCombinations(
        definition,
        active,
        0U,
        parameters,
        [&](const ParameterValues& combination) {
            const BacktestMetrics metrics = runSingleBacktest(
                definition,
                combination,
                ohlcvData,
                initialBalance,
                feeMaker,
                feeTaker,
                sensitivityMetricsSettings
            );
            results << ++runId << ',' << metrics.netReturnPercent << ','
                    << metrics.maxDrawdownPercent << ',' << metrics.tradeCount;
            for (const auto& [key, value] : combination) {
                (void)key;
                results << ',' << value;
            }
            results << '\n';

            for (std::size_t parameterIndex = 0U;
                 parameterIndex < active.size();
                 ++parameterIndex) {
                const double value = parameterOr(
                    combination,
                    active[parameterIndex].definition->key,
                    0.0
                );
                const auto valueIndex = valueIndexes[parameterIndex].find(value);
                if (valueIndex != valueIndexes[parameterIndex].end()) {
                    accumulators[parameterIndex][valueIndex->second].add(metrics);
                }
            }

            progress.advance();
        }
    );

    std::vector<ParameterSensitivityReport> reports;
    reports.reserve(active.size());

    for (std::size_t parameterIndex = 0U;
         parameterIndex < active.size();
         ++parameterIndex) {
        const ActiveSensitivityParameter& parameter = active[parameterIndex];
        ParameterSensitivityReport report;
        report.parameterName = parameter.definition->key;
        report.displayName = parameter.definition->displayName;
        report.points.reserve(parameter.values.size());

        for (std::size_t valueIndex = 0U;
             valueIndex < parameter.values.size();
             ++valueIndex) {
            const SensitivityAccumulator& accumulator =
                accumulators[parameterIndex][valueIndex];

            report.points.push_back(ParameterSensitivityPoint{
                parameter.values[valueIndex],
                median(accumulator.finalReturnPercentValues),
                median(accumulator.maxDrawdownPercentValues),
                median(accumulator.tradeCountValues),
                accumulator.positiveCombinationCount,
                accumulator.finalReturnPercentValues.size(),
                accumulator.zeroTradeCombinationCount
            });
        }

        reports.push_back(std::move(report));
    }

    return reports;
}

BacktestMetrics runStrategyAndWriteReport(
    const StrategyDefinition& definition,
    const OHLCVData& ohlcvData,
    double initialBalance,
    double feeMaker,
    double feeTaker,
    const BacktestMetricsSettings& metricsSettings,
    const std::filesystem::path& reportPath,
    SensitivityProgressReporter* progress
)
{
    (void)feeMaker;
    BacktestContext context(ohlcvData, makeStudyPortfolio(definition, definition.currentParameters), initialBalance, feeTaker);
    Backtester tester(context);
    tester.loop();
    tester.closeTrades();

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
        initialBalance,
        metricsSettings
    );

    std::vector<ParameterSensitivityReport> parameterSensitivity;
    if (progress != nullptr) {
        parameterSensitivity = runParameterSensitivity(
            definition,
            ohlcvData,
            initialBalance,
            feeMaker,
            feeTaker,
            metricsSettings,
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
    Logger::Instance().Setup(false, false, "", "", true);
    const OHLCVData data = loadDatabase(databasePath, startDate, endDate);
    if (data.data.empty())
        throw std::runtime_error("No market data in the selected window");
    const auto directory = std::filesystem::path(outputRoot) / studyId;
    std::filesystem::create_directories(directory.parent_path());
    if (!std::filesystem::create_directory(directory))
        throw std::runtime_error("Study directory already exists: " + directory.string());
    compactSensitivity = grid == "compact";
    std::size_t runs = 0;
    if (grid != "none") {
        for (const auto& definition : definitions)
            runs += countSensitivityCombinations(definition);
    }
    std::unique_ptr<SensitivityProgressReporter> progress;
    if (runs > 0)
        progress = std::make_unique<SensitivityProgressReporter>(runs);
    BacktestMetricsSettings settings{.periodsPerYear = 365.0, .excludeSimulatedTrades = true,
        .databaseTimeframe = "1d", .annualizationBenchmarkSymbol = "BTC"};
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
        const auto metrics = runStrategyAndWriteReport(definition, data, 100000.0, 0.0, feeTaker,
            settings, directory / (definition.name + ".html"), progress.get());
        logBacktestMetrics(metrics);
        summary << definition.name << ',' << metrics.netReturnPercent << ',' << metrics.maxDrawdownPercent
                << ',' << metrics.tradeCount << ',' << metrics.finalEquity << '\n';
    }
    if (progress)
        progress->finish();
    std::cout << "CURRENT initial parameter study: " << directory << '\n';
    return 0;
}
catch (const std::exception& error)
{
    std::cerr << "Initial parameter study failed: " << error.what() << '\n';
    return 1;
}
