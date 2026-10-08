#include "backtester.h"
#include "research_strategy_definitions.h"
#include <boost/program_options.hpp>
#include "backtest_metrics.h"
#include "backtest_html_report.h"
#include "database_utils.h"
#include "indicator_ranker.h"
#include "indicator_spec.h"
#include "liquidity_universe.h"
#include "logger.h"
#include "universe_selector.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <ctime>
#include <exception>
#include <stdexcept>
#include <system_error>
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

namespace {

using research_studies::ParameterValues;
using research_studies::SensitivityParameterDefinition;
using research_studies::StrategyDefinition;
using research_studies::parameterOr;

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

struct SensitivityRunStats {
    std::size_t plannedRunCount = 0U;
    std::size_t successfulRunCount = 0U;
    std::size_t invalidRunCount = 0U;
    std::size_t failedRunCount = 0U;
};

struct ParameterSensitivityExecution {
    std::vector<ParameterSensitivityReport> reports;
    SensitivityRunStats stats;
};

struct StrategyExecutionResult {
    BacktestMetrics metrics;
    SensitivityRunStats sensitivityStats;
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

std::vector<StrategyDefinition> makeStrategyDefinitions()
{
    auto definitions = research_studies::makeStrategyDefinitions();
    std::erase_if(definitions, [](const auto& definition) {
        return definition.name != "MRShort" && definition.name != "PureMom";
    });
    // Reuse the current signal and portfolio configurations, while preserving
    // this study's original fixed parameters and enabled BTC SMA sweep.
    for (auto& definition : definitions) {
        if (definition.name == "MRShort") {
            definition.sensitivityParameters = {
                {"rsiLength", "RSI length", 5.0, 5.0, 1.0, false},
                {"rsiEntry", "RSI entry", 70.0, 70.0, 1.0, false},
                {"heldBars", "Held bars", 3.0, 3.0, 1.0, false},
                {"entryAtrMultiple", "Entry ATR multiple", 0.30, 0.30, 0.10, false},
                {"entryAtrLength", "Entry ATR length", 5.0, 5.0, 1.0, false},
                {"btcMovingAverageLength", "BTC moving-average length", 10.0, 200.0, 5.0, true},
                {"rankerRocLength", "Ranker ROC length", 30.0, 30.0, 1.0, false},
                {"quantityPercent", "Quantity (%)", 10.0, 10.0, 1.0, false},
                {"maxPositionsOpen", "Maximum open positions", 10.0, 10.0, 1.0, false}
            };
        }
        if (definition.name == "PureMom") {
            definition.sensitivityParameters = {
                {"heldBars", "Held bars", 7.0, 7.0, 1.0, false},
                {"rocLength", "ROC length", 7.0, 7.0, 1.0, false},
                {"btcMovingAverageLength", "BTC moving-average length", 10.0, 200.0, 5.0, true},
                {"quantityPercent", "Quantity (%)", 10.0, 10.0, 1.0, false},
                {"maxPositionsOpen", "Maximum open positions", 3.0, 3.0, 1.0, false}
            };
        }
    }
    return definitions;
}

bool compactSensitivity = false;

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

        if (compactSensitivity && values.size() > 2U) {
            values = {values.front(), values.back()};
        }
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
    Callback& callback
)
{
    if (parameterIndex >= activeParameters.size()) {
        callback(parameters);
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
            callback
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

    auto counter = [&count](const ParameterValues&) { ++count; };

    enumerateSensitivityCombinations(
        definition,
        active,
        0U,
        parameters,
        counter
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
    (void)feeMaker; // This study submits taker orders only.
    BacktestContext context(
        ohlcvData, makeStudyPortfolio(definition, parameters), initialBalance, feeTaker
    );

    Backtester tester(context);
    tester.loop();

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

const SensitivityParameterDefinition* findSensitivityParameterDefinition(
    const StrategyDefinition& definition,
    const std::string& key
)
{
    for (const SensitivityParameterDefinition& parameter :
         definition.sensitivityParameters) {
        if (parameter.key == key) {
            return &parameter;
        }
    }

    return nullptr;
}

std::string formatCsvNumber(double value)
{
    if (!std::isfinite(value)) {
        return {};
    }

    std::ostringstream output;
    output << std::setprecision(15) << value;
    return output.str();
}

void writeCsvRow(
    std::ostream& output,
    const std::vector<std::string>& cells
)
{
    for (std::size_t index = 0U; index < cells.size(); ++index) {
        if (index > 0U) {
            output << ',';
        }

        output << '"';
        for (const char character : cells[index]) {
            if (character == '"') {
                output << "\"\"";
            } else {
                output << character;
            }
        }
        output << '"';
    }
    output << '\n';
}

std::string utcTimestamp()
{
    const std::time_t now = std::time(nullptr);
    std::tm utcTime{};

#if defined(_WIN32)
    gmtime_s(&utcTime, &now);
#else
    gmtime_r(&now, &utcTime);
#endif

    std::ostringstream output;
    output << std::put_time(&utcTime, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

class SensitivityCsvWriter {
public:
    SensitivityCsvWriter(
        const std::filesystem::path& outputPath,
        const std::string& studyId,
        const StrategyDefinition& definition
    )
        : output_(outputPath),
          studyId_(studyId),
          strategyName_(definition.name)
    {
        if (!output_.is_open()) {
            throw std::runtime_error(
                "Could not write sensitivity CSV: " + outputPath.string()
            );
        }

        parameterKeys_.reserve(definition.currentParameters.size());
        for (const auto& [key, value] : definition.currentParameters) {
            parameterKeys_.push_back(key);
            (void)value;
        }

        std::vector<std::string> header{
            "study_id",
            "strategy",
            "run_id",
            "status",
            "failure_reason"
        };
        header.insert(header.end(), parameterKeys_.begin(), parameterKeys_.end());

        const std::vector<std::string> metricHeaders{
            "net_return_percent",
            "annualized_return_percent",
            "max_drawdown_percent",
            "sharpe_ratio",
            "sortino_ratio",
            "calmar_ratio",
            "profit_factor",
            "expectancy_per_trade",
            "trade_count",
            "exposure_percent",
            "turnover_multiple",
            "win_rate_percent",
            "average_win",
            "average_loss",
            "max_consecutive_losses",
            "worst_loss_streak",
            "average_holding_bars",
            "net_profit",
            "final_equity"
        };
        header.insert(header.end(), metricHeaders.begin(), metricHeaders.end());

        writeCsvRow(output_, header);
    }

    void writeRun(
        std::size_t runId,
        const std::string& status,
        const std::string& failureReason,
        const ParameterValues& parameters,
        const BacktestMetrics* metrics
    )
    {
        std::vector<std::string> row{
            studyId_,
            strategyName_,
            std::to_string(runId),
            status,
            failureReason
        };

        for (const std::string& key : parameterKeys_) {
            const auto iterator = parameters.find(key);
            row.push_back(
                iterator == parameters.end()
                    ? std::string{}
                    : formatCsvNumber(iterator->second)
            );
        }

        if (metrics != nullptr) {
            row.insert(
                row.end(),
                {
                    formatCsvNumber(metrics->netReturnPercent),
                    formatCsvNumber(metrics->annualizedReturnPercent),
                    formatCsvNumber(metrics->maxDrawdownPercent),
                    formatCsvNumber(metrics->sharpeRatio),
                    formatCsvNumber(metrics->sortinoRatio),
                    formatCsvNumber(metrics->calmarRatio),
                    formatCsvNumber(metrics->profitFactor),
                    formatCsvNumber(metrics->expectancyPerTrade),
                    std::to_string(metrics->tradeCount),
                    formatCsvNumber(metrics->exposurePercent),
                    formatCsvNumber(metrics->turnoverMultiple),
                    formatCsvNumber(metrics->winRatePercent),
                    formatCsvNumber(metrics->averageWin),
                    formatCsvNumber(metrics->averageLoss),
                    std::to_string(metrics->maximumConsecutiveLosses),
                    formatCsvNumber(metrics->worstConsecutiveLossPnl),
                    formatCsvNumber(metrics->averageHoldingBars),
                    formatCsvNumber(metrics->netProfit),
                    formatCsvNumber(metrics->finalEquity)
                }
            );
        } else {
            constexpr std::size_t metricColumnCount = 19U;
            row.insert(row.end(), metricColumnCount, std::string{});
        }

        writeCsvRow(output_, row);
    }

private:
    std::ofstream output_;
    std::string studyId_;
    std::string strategyName_;
    std::vector<std::string> parameterKeys_;
};

std::string invalidCombinationReason(
    const StrategyDefinition& definition,
    const ParameterValues& parameters
)
{
    for (const auto& [key, value] : parameters) {
        if (!std::isfinite(value)) {
            return "Non-finite parameter value for '" + key + "'";
        }
    }

    if (definition.name == "PureRSI" &&
        parameterOr(parameters, "rsiEntry", 0.0) <=
        parameterOr(parameters, "rsiExit", 0.0)) {
        return "rsiEntry must be strictly greater than rsiExit";
    }

    if (definition.isValidCombination &&
        !definition.isValidCombination(parameters)) {
        return "Rejected by the strategy-specific valid-combination predicate";
    }

    return {};
}

ParameterSensitivityExecution runParameterSensitivity(
    const StrategyDefinition& definition,
    const OHLCVData& ohlcvData,
    double initialBalance,
    double feeMaker,
    double feeTaker,
    const BacktestMetricsSettings& baseMetricsSettings,
    const std::filesystem::path& csvPath,
    const std::string& studyId,
    SensitivityProgressReporter& progress
)
{
    ParameterSensitivityExecution execution;

    const std::vector<ActiveSensitivityParameter> active =
        activeSensitivityParameters(definition);

    if (active.empty()) {
        return execution;
    }

    // Sensitivity runs use historical metrics only. Monte Carlo is disabled
    // here so every grid point remains a single deterministic backtest.
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

    SensitivityCsvWriter csvWriter(csvPath, studyId, definition);

    ParameterValues parameters = definition.currentParameters;
    std::size_t runId = 0U;
    progress.setStrategy(definition.name);

    auto processCombination = [&](const ParameterValues& combination) {
        ++runId;
        ++execution.stats.plannedRunCount;

        const std::string validationReason =
            invalidCombinationReason(definition, combination);

        if (!validationReason.empty()) {
            ++execution.stats.invalidRunCount;
            csvWriter.writeRun(
                runId,
                "invalid",
                validationReason,
                combination,
                nullptr
            );
            progress.advance();
            return;
        }

        try {
            const BacktestMetrics metrics = runSingleBacktest(
                definition,
                combination,
                ohlcvData,
                initialBalance,
                feeMaker,
                feeTaker,
                sensitivityMetricsSettings
            );

            csvWriter.writeRun(
                runId,
                "success",
                {},
                combination,
                &metrics
            );
            ++execution.stats.successfulRunCount;

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
        } catch (const std::exception& exception) {
            ++execution.stats.failedRunCount;
            csvWriter.writeRun(
                runId,
                "failed",
                exception.what(),
                combination,
                nullptr
            );
            LG_ERROR(
                "Sensitivity run failed for {} (run {}): {}",
                definition.name,
                runId,
                exception.what()
            );
        } catch (...) {
            ++execution.stats.failedRunCount;
            csvWriter.writeRun(
                runId,
                "failed",
                "Unknown non-standard exception",
                combination,
                nullptr
            );
            LG_ERROR(
                "Sensitivity run failed for {} (run {}): unknown exception",
                definition.name,
                runId
            );
        }

        progress.advance();
    };

    enumerateSensitivityCombinations(
        definition,
        active,
        0U,
        parameters,
        processCombination
    );

    execution.reports.reserve(active.size());
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

        execution.reports.push_back(std::move(report));
    }

    return execution;
}

StrategyExecutionResult runStrategyAndWriteReport(
    const StrategyDefinition& definition,
    const OHLCVData& ohlcvData,
    double initialBalance,
    double feeMaker,
    double feeTaker,
    const BacktestMetricsSettings& metricsSettings,
    const std::filesystem::path& reportPath,
    const std::filesystem::path& sensitivityCsvPath,
    const std::string& studyId,
    SensitivityProgressReporter* progress
)
{
    StrategyExecutionResult result;

    (void)feeMaker; // This study submits taker orders only.
    BacktestContext context(
        ohlcvData, makeStudyPortfolio(definition, definition.currentParameters), initialBalance, feeTaker
    );

    Backtester tester(context);
    tester.loop();

    result.metrics = calculateBacktestMetrics(
        definition.name,
        context.GetTradesHistory(),
        context.GetBalanceEquityHistoric(),
        context.GetMarketData(),
        initialBalance,
        metricsSettings
    );

    const auto reportDirectory = reportPath.parent_path();
    tester.storeTradesCSV(reportDirectory / (definition.name + "_trades.csv"));
    std::ofstream accountOutput(reportDirectory / (definition.name + "_account.csv"));
    accountOutput.exceptions(std::ios::failbit | std::ios::badbit);
    accountOutput << "timestamp,cash,balance,equity\n" << std::setprecision(17);
    for (const auto& snapshot : context.GetAccountHistory()) {
        accountOutput << snapshot.timestamp << ',' << snapshot.cash << ','
                      << snapshot.balance << ',' << snapshot.equity << '\n';
    }
    SensitivityCsvWriter baselineWriter(
        reportDirectory / (definition.name + "_metrics.csv"), studyId, definition
    );
    baselineWriter.writeRun(0, "success", {}, definition.currentParameters, &result.metrics);

    std::vector<ParameterSensitivityReport> parameterSensitivity;
    if (progress != nullptr) {
        ParameterSensitivityExecution sensitivity = runParameterSensitivity(
            definition,
            ohlcvData,
            initialBalance,
            feeMaker,
            feeTaker,
            metricsSettings,
            sensitivityCsvPath,
            studyId,
            *progress
        );
        parameterSensitivity = std::move(sensitivity.reports);
        result.sensitivityStats = sensitivity.stats;
    }

    if (!writeBacktestHtmlReport(
            reportPath,
            result.metrics,
            context.GetBalanceEquityHistoric(),
            context.GetMarketData(),
            parameterSensitivity
        )) {
        throw std::runtime_error("Could not write report for strategy " + definition.name);
    }

    return result;
}

bool ensureDirectoryExists(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::create_directories(path, error);

    if (error) {
        LG_ERROR(
            "Could not create output directory '{}': {}",
            path.string(),
            error.message()
        );
        return false;
    }

    return true;
}

void writeParameterGridCsv(
    const std::filesystem::path& outputPath,
    const std::string& studyId,
    const std::vector<StrategyDefinition>& definitions
)
{
    std::ofstream output(outputPath);
    if (!output.is_open()) {
        throw std::runtime_error(
            "Could not write parameter grid CSV: " + outputPath.string()
        );
    }

    writeCsvRow(
        output,
        {
            "study_id",
            "strategy",
            "parameter_name",
            "display_name",
            "current_value",
            "enabled",
            "minimum",
            "maximum",
            "spacing"
        }
    );

    for (const StrategyDefinition& definition : definitions) {
        for (const auto& [key, currentValue] : definition.currentParameters) {
            const SensitivityParameterDefinition* sensitivityDefinition =
                findSensitivityParameterDefinition(definition, key);

            writeCsvRow(
                output,
                {
                    studyId,
                    definition.name,
                    key,
                    sensitivityDefinition == nullptr
                        ? std::string{}
                        : sensitivityDefinition->displayName,
                    formatCsvNumber(currentValue),
                    sensitivityDefinition != nullptr &&
                        sensitivityDefinition->enabled
                        ? "yes"
                        : "no",
                    sensitivityDefinition == nullptr
                        ? std::string{}
                        : formatCsvNumber(sensitivityDefinition->minimum),
                    sensitivityDefinition == nullptr
                        ? std::string{}
                        : formatCsvNumber(sensitivityDefinition->maximum),
                    sensitivityDefinition == nullptr
                        ? std::string{}
                        : formatCsvNumber(sensitivityDefinition->spacing)
                }
            );
        }
    }
}

void writeStudyMetadataCsv(
    const std::filesystem::path& outputPath,
    const std::string& studyId,
    const std::string& databasePath,
    double initialBalance,
    double feeMaker,
    double feeTaker,
    double commissionEntryFactor,
    double commissionExitFactor,
    const BacktestMetricsSettings& metricsSettings,
    const std::vector<std::pair<std::string, SensitivityRunStats>>& strategyStats
)
{
    std::ofstream output(outputPath);
    if (!output.is_open()) {
        throw std::runtime_error(
            "Could not write study metadata CSV: " + outputPath.string()
        );
    }

    writeCsvRow(
        output,
        {
            "record_type",
            "study_id",
            "created_utc",
            "database_path",
            "initial_balance",
            "fee_maker",
            "fee_taker",
            "commission_entry_factor",
            "commission_exit_factor",
            "periods_per_year",
            "exclude_simulated_trades",
            "sensitivity_monte_carlo_enabled",
            "strategy",
            "planned_runs",
            "successful_runs",
            "invalid_runs",
            "failed_runs"
        }
    );

    const std::string createdUtc = utcTimestamp();

    writeCsvRow(
        output,
        {
            "study",
            studyId,
            createdUtc,
            databasePath,
            formatCsvNumber(initialBalance),
            formatCsvNumber(feeMaker),
            formatCsvNumber(feeTaker),
            formatCsvNumber(commissionEntryFactor),
            formatCsvNumber(commissionExitFactor),
            formatCsvNumber(metricsSettings.periodsPerYear),
            metricsSettings.excludeSimulatedTrades ? "yes" : "no",
            "no",
            {},
            {},
            {},
            {},
            {}
        }
    );

    for (const auto& [strategyName, stats] : strategyStats) {
        writeCsvRow(
            output,
            {
                "strategy",
                studyId,
                createdUtc,
                databasePath,
                formatCsvNumber(initialBalance),
                formatCsvNumber(feeMaker),
                formatCsvNumber(feeTaker),
                formatCsvNumber(commissionEntryFactor),
                formatCsvNumber(commissionExitFactor),
                formatCsvNumber(metricsSettings.periodsPerYear),
                metricsSettings.excludeSimulatedTrades ? "yes" : "no",
                "no",
                strategyName,
                std::to_string(stats.plannedRunCount),
                std::to_string(stats.successfulRunCount),
                std::to_string(stats.invalidRunCount),
                std::to_string(stats.failedRunCount)
            }
        );
    }
}

} // namespace

int main(int argc, char** argv)
try
{
    namespace options = boost::program_options;
    std::string databasePath;
    const std::string databaseTimeframe = "1d";
    const std::string benchmarkSymbol = "BTC";
    std::string studyId;
    std::string outputRoot;
    std::string grid;
    std::string selectedStrategy;
    Timestamp startDate;
    Timestamp endDate;
    double feeTaker;
    options::options_description arguments("BTC moving-average study options");
    arguments.add_options()
        ("help,h", "Show study options")
        ("database", options::value(&databasePath)->default_value("storage/databases/1d_cmc.csv"), "Input CSV/SQLite")
        ("start", options::value(&startDate)->default_value(20200101), "Cold start YYYYMMDD (inclusive)")
        ("end", options::value(&endDate)->default_value(20200416), "Cutoff YYYYMMDD (inclusive)")
        ("study-id", options::value(&studyId)->default_value(""), "New study directory name (default UTC timestamp)")
        ("output-root", options::value(&outputRoot)->default_value("storage/backtests/sensitivity_results"), "Study parent directory")
        ("strategy", options::value(&selectedStrategy)->default_value("all"), "Strategy name or all")
        ("grid", options::value(&grid)->default_value("none"), "none, compact or full sensitivity grid")
        ("fee-taker", options::value(&feeTaker)->default_value(0.0), "Market commission fraction per fill");
    try {
        options::variables_map values;
        options::store(options::parse_command_line(argc, argv, arguments), values);
        if (values.count("help")) {
            std::cout << arguments << '\n';
            return 0;
        }
        options::notify(values);
        auto validDate = [](Timestamp date) {
            const std::chrono::year_month_day calendarDate{
                std::chrono::year(static_cast<int>(date / 10000)),
                std::chrono::month((date / 100) % 100),
                std::chrono::day(date % 100)
            };
            return date >= 19000101 && date <= 99991231 && calendarDate.ok();
        };
        if (!validDate(startDate) || !validDate(endDate) || startDate > endDate)
            throw std::invalid_argument("Require valid start <= end dates in YYYYMMDD format");
        if (grid != "none" && grid != "compact" && grid != "full")
            throw std::invalid_argument("Grid must be none, compact or full");
        if (!std::isfinite(feeTaker) || feeTaker < 0.0)
            throw std::invalid_argument("Fee must be finite and non-negative");
        if (studyId.empty()) {
            studyId = utcTimestamp();
            studyId.erase(std::remove(studyId.begin(), studyId.end(), ':'), studyId.end());
            studyId = "current_btc_ma_" + studyId;
        }
        if (studyId == "." || studyId == ".." || studyId.find_first_of("/\\") != std::string::npos)
            throw std::invalid_argument("Study ID must be a single directory name");
    } catch (const std::exception& exception) {
        std::cerr << "Invalid study options: " << exception.what() << '\n';
        return 1;
    }

    Logger::Instance().Setup(
        true,   // debug enabled
        false,  // quiet
        "",     // file appender
        "",     // rolling appender
        true    // include header
    );

    const double periodsPerYear = 365.0;
    constexpr double initialBalance = 100000.0;
    constexpr double feeMaker = 0.0;
    // Retained metadata columns describe the former strategy-side fee factors.
    // CURRENT commissions come from actual taker fills, never these factors.
    constexpr double commissionEntryFactor = 0.0;
    constexpr double commissionExitFactor = 0.0;
    const bool runParameterSensitivity = grid != "none";

    const BacktestMetricsSettings metricsSettings{
        .periodsPerYear = periodsPerYear,
        .excludeSimulatedTrades = true,
        .databaseTimeframe = databaseTimeframe,
        .annualizationBenchmarkSymbol = benchmarkSymbol
    };

    const std::filesystem::path sensitivityRootDirectory = outputRoot;

    const std::filesystem::path studyDirectory =
        sensitivityRootDirectory / studyId;

    const std::filesystem::path reportsDirectory =
        studyDirectory / "reports";

    if (std::filesystem::exists(studyDirectory))
        throw std::invalid_argument("Study directory already exists; choose a new study ID");

    if (!ensureDirectoryExists(studyDirectory) ||
        !ensureDirectoryExists(reportsDirectory)) {
        return 1;
    }

    LG_INFO("Database loading started");
    if (!std::filesystem::is_regular_file(databasePath))
        throw std::invalid_argument("Input database does not exist: " + databasePath);
    OHLCVData ohlcvData = loadDatabase(databasePath, startDate, endDate);
    if (ohlcvData.data.empty() || !ohlcvData.data.contains(benchmarkSymbol))
        throw std::invalid_argument("Selected window must contain market data and the annualization benchmark");
    LG_INFO("Database loaded successfully");
    LG_INFO(
        "Annualization settings: timeframe={} benchmark_symbol={} periods_per_year={}",
        databaseTimeframe,
        benchmarkSymbol,
        periodsPerYear
    );

    std::vector<StrategyDefinition> strategyDefinitions = makeStrategyDefinitions();
    if (selectedStrategy != "all") {
        std::erase_if(strategyDefinitions, [&](const auto& definition) {
            return definition.name != selectedStrategy;
        });
    }
    if (strategyDefinitions.empty())
        throw std::invalid_argument("Unknown BTC study strategy: " + selectedStrategy);
    compactSensitivity = grid == "compact";
    std::ofstream executionSettings(studyDirectory / "execution_settings.csv");
    executionSettings.exceptions(std::ios::failbit | std::ios::badbit);
    executionSettings << "runtime,start,end,grid,cutoff_policy\n"
                      << "current-lib," << startDate << ',' << endDate << ','
                      << grid << ",mark-open-positions\n";

    try {
        writeParameterGridCsv(
            studyDirectory / "parameter_grid.csv",
            studyId,
            strategyDefinitions
        );
    } catch (const std::exception& exception) {
        LG_ERROR("{}", exception.what());
        return 1;
    }

    std::size_t totalSensitivityRuns = 0U;
    if (runParameterSensitivity) {
        for (const StrategyDefinition& definition : strategyDefinitions) {
            const std::size_t strategyRuns = countSensitivityCombinations(definition);
            totalSensitivityRuns += strategyRuns;
            LG_INFO(
                "Sensitivity grid {}: {} planned parameter combinations",
                definition.name,
                strategyRuns
            );
        }
        LG_INFO(
            "Total parameter-sensitivity grid: {} planned parameter combinations",
            totalSensitivityRuns
        );
    }

    std::unique_ptr<SensitivityProgressReporter> progress;
    if (runParameterSensitivity && totalSensitivityRuns > 0U) {
        progress = std::make_unique<SensitivityProgressReporter>(
            totalSensitivityRuns
        );
    }

    std::map<std::string, BacktestMetrics> metricsCache;
    std::vector<std::pair<std::string, SensitivityRunStats>> sensitivityStats;
    sensitivityStats.reserve(strategyDefinitions.size());

    try {
        for (const StrategyDefinition& definition : strategyDefinitions) {
            LG_INFO("============================================================");
            LG_INFO("Running isolated backtest for strategy: {}", definition.name);

            const StrategyExecutionResult result = runStrategyAndWriteReport(
                definition,
                ohlcvData,
                initialBalance,
                feeMaker,
                feeTaker,
                metricsSettings,
                reportsDirectory / (definition.name + ".html"),
                studyDirectory / (definition.name + ".csv"),
                studyId,
                progress.get()
            );

            if (progress) {
                std::cout << '\n';
            }
            logBacktestMetrics(result.metrics);
            metricsCache.insert_or_assign(definition.name, result.metrics);
            sensitivityStats.emplace_back(
                definition.name,
                result.sensitivityStats
            );

            if (runParameterSensitivity) {
                LG_INFO(
                    "{} sensitivity: {} success, {} invalid, {} failed",
                    definition.name,
                    result.sensitivityStats.successfulRunCount,
                    result.sensitivityStats.invalidRunCount,
                    result.sensitivityStats.failedRunCount
                );
            }
        }

        if (progress) {
            progress->finish();
        }

        writeStudyMetadataCsv(
            studyDirectory / "study_metadata.csv",
            studyId,
            databasePath,
            initialBalance,
            feeMaker,
            feeTaker,
            commissionEntryFactor,
            commissionExitFactor,
            metricsSettings,
            sensitivityStats
        );
    } catch (const std::exception& exception) {
        if (progress) {
            progress->finish();
        }
        LG_ERROR("Backtest study stopped: {}", exception.what());
        return 1;
    }

    LG_INFO("============================================================");
    LG_INFO(
        "Completed {} baseline strategy backtests. Study outputs are in: {}",
        metricsCache.size(),
        studyDirectory.string()
    );

    for (const auto& [strategyName, metrics] : metricsCache) {
        LG_INFO(
            "Summary {}: return={:.2f}% annualized={:.2f}% max_dd={:.2f}% "
            "sharpe={:.3f} sortino={:.3f} calmar={:.3f} "
            "profit_factor={:.3f} trades={}",
            strategyName,
            metrics.netReturnPercent,
            metrics.annualizedReturnPercent,
            metrics.maxDrawdownPercent,
            metrics.sharpeRatio,
            metrics.sortinoRatio,
            metrics.calmarRatio,
            metrics.profitFactor,
            metrics.tradeCount
        );
    }

    return 0;
}
catch (const std::exception& exception)
{
    std::cerr << "BTC moving-average study failed: " << exception.what() << '\n';
    return 1;
}
