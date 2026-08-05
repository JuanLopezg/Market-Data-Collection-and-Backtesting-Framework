#include "backtest.h"
#include "all_strategies.h"
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

using ParameterValues = std::map<std::string, double>;

struct SensitivityParameterDefinition {
    std::string key;
    std::string displayName;
    double minimum = 0.0;
    double maximum = 0.0;
    double spacing = 1.0;
    bool enabled = false;
};

struct StrategyDefinition {
    std::string name;
    ParameterValues currentParameters;
    std::vector<SensitivityParameterDefinition> sensitivityParameters;
    std::function<std::unique_ptr<Strategy>(const ParameterValues&)> create;
    std::function<bool(const ParameterValues&)> isValidCombination;
};

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

double parameterOr(
    const ParameterValues& parameters,
    const std::string& key,
    double fallback
)
{
    const auto iterator = parameters.find(key);
    return iterator == parameters.end() ? fallback : iterator->second;
}

unsigned int unsignedParameterOr(
    const ParameterValues& parameters,
    const std::string& key,
    unsigned int fallback
)
{
    const double value = parameterOr(
        parameters,
        key,
        static_cast<double>(fallback)
    );

    if (!std::isfinite(value) || value <= 0.0) {
        return 0U;
    }

    const double upperBound = static_cast<double>(
        std::numeric_limits<unsigned int>::max()
    );
    return static_cast<unsigned int>(std::llround(std::min(value, upperBound)));
}

std::unique_ptr<UniverseSelector> makeTopLiquidityUniverse()
{
    constexpr unsigned int topLiquidityCount = 20;

    return std::make_unique<TopNLiquidityUniverse>(
        IndicatorSpec{
            IndicatorKind::SMA,
            PriceField::Volume,
            25
        },
        topLiquidityCount,
        true
    );
}

std::vector<SensitivityParameterDefinition> makeSensitivityParameters(
    std::initializer_list<SensitivityParameterDefinition> parameters
)
{
    return std::vector<SensitivityParameterDefinition>(
        parameters.begin(),
        parameters.end()
    );
}

std::vector<StrategyDefinition> makeStrategyDefinitions(
    double feeMaker,
    double feeTaker,
    double commissionEntryFactor,
    double commissionExitFactor
)
{
    std::vector<StrategyDefinition> definitions;

    definitions.push_back(StrategyDefinition{
        "MRShort",
        {
            {"rsiLength", 5.0},
            {"rsiEntry", 70.0},
            {"btcMovingAverageLength", 50.0},
            {"entryAtrMultiple", 0.30},
            {"entryAtrLength", 5.0},
            {"heldBars", 3.0},
            {"quantityPercent", 10.0},
            {"maxPositionsOpen", 10.0},
            {"maxRankingPosition", 1000000.0},
            {"rankerRocLength", 30.0}
        },
        makeSensitivityParameters({
            {"rsiLength", "RSI length", 5.0, 5.0, 1.0, false},
            {"rsiEntry", "RSI entry", 70.0, 70.0, 1.0, false},
            {"heldBars", "Held bars", 3.0, 3.0, 1.0, false},
            {"entryAtrMultiple", "Entry ATR multiple", 0.30, 0.30, 0.10, false},
            {"entryAtrLength", "Entry ATR length", 5.0, 5.0, 1.0, false},

            {"btcMovingAverageLength",
            "BTC moving-average length",
            10.0,
            200.0,
            5.0,
            true},

            {"rankerRocLength", "Ranker ROC length", 30.0, 30.0, 1.0, false},
            {"quantityPercent", "Quantity (%)", 10.0, 10.0, 1.0, false},
            {"maxPositionsOpen", "Maximum open positions", 10.0, 10.0, 1.0, false}
        }),
        [=](const ParameterValues& parameters) {
            const unsigned int rsiLength = unsignedParameterOr(
                parameters,
                "rsiLength",
                5U
            );

            const double rsiEntry = parameterOr(
                parameters,
                "rsiEntry",
                70.0
            );

            const unsigned int btcMovingAverageLength = unsignedParameterOr(
                parameters,
                "btcMovingAverageLength",
                50U
            );

            const double entryAtrMultiple = parameterOr(
                parameters,
                "entryAtrMultiple",
                0.30
            );

            const unsigned int entryAtrLength = unsignedParameterOr(
                parameters,
                "entryAtrLength",
                5U
            );

            const unsigned int heldBars = unsignedParameterOr(
                parameters,
                "heldBars",
                3U
            );

            const double quantityPercent = parameterOr(
                parameters,
                "quantityPercent",
                10.0
            );

            const unsigned int maxPositionsOpen = unsignedParameterOr(
                parameters,
                "maxPositionsOpen",
                10U
            );

            const unsigned int maxRankingPosition = unsignedParameterOr(
                parameters,
                "maxRankingPosition",
                1000000U
            );

            const unsigned int rankerRocLength = unsignedParameterOr(
                parameters,
                "rankerRocLength",
                30U
            );

            auto universeSelector = makeTopLiquidityUniverse();

            auto ranker = std::make_unique<IndicatorRanker>(
                IndicatorSpec{
                    IndicatorKind::ROC,
                    PriceField::Close,
                    rankerRocLength
                },
                true
            );

            return std::make_unique<StrategyMRShort>(
                maxPositionsOpen,
                quantityPercent / 100.0,
                std::move(universeSelector),
                std::move(ranker),
                feeTaker,
                feeTaker,
                maxRankingPosition,
                rsiLength,
                rsiEntry,
                btcMovingAverageLength,
                entryAtrMultiple,
                entryAtrLength,
                heldBars,
                "BTC"
            );
        },
        [](const ParameterValues&) {
            return true;
        }
    });


    definitions.push_back(StrategyDefinition{
        "PureMom",
        {
            {"heldBars", 7.0},
            {"rocLength", 7.0},
            {"btcMovingAverageLength", 50.0},
            {"quantityPercent", 10.0},
            {"maxPositionsOpen", 3.0},
            {
                "maxRankingPosition",
                static_cast<double>(
                    std::numeric_limits<unsigned int>::max()
                )
            }
        },
        makeSensitivityParameters({
            {"heldBars", "Held bars", 7.0, 7.0, 1.0, false},
            {"rocLength", "ROC length", 7.0, 7.0, 1.0, false},

            {"btcMovingAverageLength",
            "BTC moving-average length",
            10.0,
            200.0,
            5.0,
            true},

            {"quantityPercent", "Quantity (%)", 10.0, 10.0, 1.0, false},
            {"maxPositionsOpen", "Maximum open positions", 3.0, 3.0, 1.0, false}
        }),
        [=](const ParameterValues& parameters) {
            const unsigned int heldBars = unsignedParameterOr(
                parameters,
                "heldBars",
                7U
            );

            const unsigned int rocLength = unsignedParameterOr(
                parameters,
                "rocLength",
                7U
            );

            const unsigned int btcMovingAverageLength = unsignedParameterOr(
                parameters,
                "btcMovingAverageLength",
                50U
            );

            const double quantityPercent = parameterOr(
                parameters,
                "quantityPercent",
                10.0
            );

            const unsigned int maxPositionsOpen = unsignedParameterOr(
                parameters,
                "maxPositionsOpen",
                3U
            );

            const unsigned int maxRankingPosition = unsignedParameterOr(
                parameters,
                "maxRankingPosition",
                std::numeric_limits<unsigned int>::max()
            );

            auto universeSelector = makeTopLiquidityUniverse();

            auto ranker = std::make_unique<IndicatorRanker>(
                IndicatorSpec{
                    IndicatorKind::ROC,
                    PriceField::Close,
                    rocLength
                },
                true
            );

            return std::make_unique<StrategyPureMom>(
                maxPositionsOpen,
                quantityPercent / 100.0,
                std::move(universeSelector),
                std::move(ranker),
                feeTaker,
                feeTaker,
                maxRankingPosition,
                heldBars,
                btcMovingAverageLength,
                "BTC"
            );
        },
        [](const ParameterValues&) {
            return true;
        }
    });

    return definitions;
}

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
    unsigned int lastTradeId = 0U;
    double balance = initialBalance;
    double equity = initialBalance;

    std::vector<std::unique_ptr<Strategy>> strategies;
    strategies.push_back(definition.create(parameters));

    BacktestContext context(
        ohlcvData,
        strategies,
        balance,
        equity,
        lastTradeId,
        feeMaker,
        feeTaker,
        false
    );

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

    unsigned int lastTradeId = 0U;
    double balance = initialBalance;
    double equity = initialBalance;

    std::vector<std::unique_ptr<Strategy>> strategies;
    strategies.push_back(definition.create(definition.currentParameters));

    BacktestContext context(
        ohlcvData,
        strategies,
        balance,
        equity,
        lastTradeId,
        feeMaker,
        feeTaker,
        false
    );

    Backtester tester(context);
    tester.loop();
    tester.closeTrades();

    result.metrics = calculateBacktestMetrics(
        definition.name,
        context.GetTradesHistory(),
        context.GetBalanceEquityHistoric(),
        context.GetMarketData(),
        initialBalance,
        metricsSettings
    );

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
        LG_ERROR("Could not write report for strategy {}", definition.name);
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
{
    (void)argc;
    (void)argv;

    Logger::Instance().Setup(
        true,   // debug enabled
        false,  // quiet
        "",     // file appender
        "",     // rolling appender
        true    // include header
    );

    const std::string databasePath =
        "/mnt/c/Users/Juan/Documents/Python/algoTrading/storage/databases/1d_cmc.csv";

    constexpr double initialBalance = 100000.0;

    // Keep identical to the validated RealTest-matching setup.
    constexpr double feeTaker = 0.0;
    constexpr double feeMaker = 0.0;
    constexpr double commissionEntryFactor = 0.0;
    constexpr double commissionExitFactor = 0.0;

    constexpr bool runParameterSensitivity = true;
    const std::string studyId = "local_refinement_btc_moving_average";

    const BacktestMetricsSettings metricsSettings{
        .periodsPerYear = 365.0,
        .excludeSimulatedTrades = true
    };

    const std::filesystem::path sensitivityRootDirectory =
        "/mnt/c/Users/Juan/Documents/Python/algoTrading/"
        "storage/backtests/sensitivity_results";

    const std::filesystem::path studyDirectory =
        sensitivityRootDirectory / studyId;

    const std::filesystem::path reportsDirectory =
        studyDirectory / "reports";

    if (!ensureDirectoryExists(studyDirectory) ||
        !ensureDirectoryExists(reportsDirectory)) {
        return 1;
    }

    LG_INFO("Database loading started");
    OHLCVData ohlcvData = loadDatabase(databasePath, 00000000);
    LG_INFO("Database loaded successfully");

    const std::vector<StrategyDefinition> strategyDefinitions =
        makeStrategyDefinitions(
            feeMaker,
            feeTaker,
            commissionEntryFactor,
            commissionExitFactor
        );

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
