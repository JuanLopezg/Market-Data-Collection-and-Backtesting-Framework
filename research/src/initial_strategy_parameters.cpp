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
        "BargainChaser",
        {
            {"maxPositionsOpen", 10.0},
            {"riskPerTrade", 0.10},
            {"maxRankingPosition", 999999.0},
            {"barsUntilExit", 2.0},
            {"fallPercentage", 10.0},
            {"movingAverageLength", 50.0},
            {"rankerRocLength", 1.0}
        },
        makeSensitivityParameters({
            {"barsUntilExit", "Bars until exit", 1.0, 20.0, 2.0, true},
            {"fallPercentage", "Fall percentage", 5.0, 20.0, 2.5, true},
            {"movingAverageLength", "Moving-average length", 20.0, 100.0, 10.0, true},
            {"rankerRocLength", "Ranker ROC length", 1.0, 30.0, 2.0, true},
            {"maxPositionsOpen", "Maximum open positions", 1.0, 20.0, 1.0, false},
            {"riskPerTrade", "Risk per trade", 0.02, 0.20, 0.02, false}
        }),
        [=](const ParameterValues& parameters) {
            const unsigned int maxPositionsOpen = unsignedParameterOr(
                parameters, "maxPositionsOpen", 10U
            );
            const double riskPerTrade = parameterOr(
                parameters, "riskPerTrade", 0.10
            );
            const unsigned int maxRankingPosition = unsignedParameterOr(
                parameters, "maxRankingPosition", 999999U
            );
            const unsigned int barsUntilExit = unsignedParameterOr(
                parameters, "barsUntilExit", 2U
            );
            const double fallPercentage = parameterOr(
                parameters, "fallPercentage", 10.0
            );
            const unsigned int movingAverageLength = unsignedParameterOr(
                parameters, "movingAverageLength", 50U
            );
            const unsigned int rankerRocLength = unsignedParameterOr(
                parameters, "rankerRocLength", 1U
            );

            auto universeSelector = makeTopLiquidityUniverse();
            auto ranker = std::make_unique<IndicatorRanker>(
                IndicatorSpec{IndicatorKind::ROC, PriceField::Close, rankerRocLength},
                false
            );

            return std::make_unique<StrategyBargainChaser>(
                maxPositionsOpen,
                riskPerTrade,
                std::move(universeSelector),
                std::move(ranker),
                commissionEntryFactor,
                commissionExitFactor,
                maxRankingPosition,
                barsUntilExit,
                fallPercentage,
                movingAverageLength
            );
        },
        [](const ParameterValues&) { return true; }
    });

    definitions.push_back(StrategyDefinition{
        "ATRBreakout",
        {
            {"heldBars", 2.0},
            {"atrMultiple", 0.75},
            {"atrLength", 3.0},
            {"momentumScoreLength", 30.0},
            {"quantityPercent", 10.0},
            {"maxPositionsOpen", 10.0},
            {"maxRankingPosition", 999999.0}
        },
        makeSensitivityParameters({
            {"heldBars", "Held bars", 1.0, 20.0, 2.0, true},
            {"atrMultiple", "ATR multiple", 0.25, 1.50, 0.25, true},
            {"atrLength", "ATR length", 1.0, 11.0, 1.0, true},
            {"momentumScoreLength", "Momentum score length", 10.0, 60.0, 5.0, true},
            {"quantityPercent", "Quantity (%)", 2.0, 20.0, 2.0, false},
            {"maxPositionsOpen", "Maximum open positions", 1.0, 20.0, 1.0, false}
        }),
        [=](const ParameterValues& parameters) {
            const unsigned int heldBars = unsignedParameterOr(parameters, "heldBars", 2U);
            const double atrMultiple = parameterOr(parameters, "atrMultiple", 0.75);
            const unsigned int atrLength = unsignedParameterOr(parameters, "atrLength", 3U);
            const unsigned int momentumScoreLength = unsignedParameterOr(
                parameters, "momentumScoreLength", 30U
            );
            const double quantityPercent = parameterOr(parameters, "quantityPercent", 10.0);
            const unsigned int maxPositionsOpen = unsignedParameterOr(
                parameters, "maxPositionsOpen", 10U
            );
            const unsigned int maxRankingPosition = unsignedParameterOr(
                parameters, "maxRankingPosition", 999999U
            );

            auto universeSelector = makeTopLiquidityUniverse();
            auto ranker = std::make_unique<IndicatorRanker>(
                IndicatorSpec{
                    IndicatorKind::ROC,
                    PriceField::Close,
                    momentumScoreLength
                },
                true
            );

            return std::make_unique<StrategyATRBreakout>(
                maxPositionsOpen,
                quantityPercent / 100.0,
                std::move(universeSelector),
                std::move(ranker),
                commissionEntryFactor,
                commissionExitFactor,
                maxRankingPosition,
                heldBars,
                atrMultiple,
                atrLength
            );
        },
        [](const ParameterValues&) { return true; }
    });

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
            {"rsiLength", "RSI length", 2.0, 14.0, 1.0, true},
            {"rsiEntry", "RSI entry", 55.0, 90.0, 5.0, true},
            {"heldBars", "Held bars", 1.0, 20.0, 2.0, true},
            {"entryAtrMultiple", "Entry ATR multiple", 0.0, 1.0, 0.1, true},
            {"entryAtrLength", "Entry ATR length", 5.0, 5.0, 1.0, false},
            {"btcMovingAverageLength", "BTC moving-average length", 50.0, 50.0, 1.0, false},
            {"rankerRocLength", "Ranker ROC length", 10.0, 60.0, 5.0, false},
            {"quantityPercent", "Quantity (%)", 2.0, 20.0, 2.0, false},
            {"maxPositionsOpen", "Maximum open positions", 1.0, 20.0, 1.0, false}
        }),
        [=](const ParameterValues& parameters) {
            const unsigned int rsiLength = unsignedParameterOr(parameters, "rsiLength", 5U);
            const double rsiEntry = parameterOr(parameters, "rsiEntry", 70.0);
            const unsigned int btcMovingAverageLength = unsignedParameterOr(
                parameters, "btcMovingAverageLength", 50U
            );
            const double entryAtrMultiple = parameterOr(
                parameters, "entryAtrMultiple", 0.30
            );
            const unsigned int entryAtrLength = unsignedParameterOr(
                parameters, "entryAtrLength", 5U
            );
            const unsigned int heldBars = unsignedParameterOr(parameters, "heldBars", 3U);
            const double quantityPercent = parameterOr(parameters, "quantityPercent", 10.0);
            const unsigned int maxPositionsOpen = unsignedParameterOr(
                parameters, "maxPositionsOpen", 10U
            );
            const unsigned int maxRankingPosition = unsignedParameterOr(
                parameters, "maxRankingPosition", 1000000U
            );
            const unsigned int rankerRocLength = unsignedParameterOr(
                parameters, "rankerRocLength", 30U
            );

            auto universeSelector = makeTopLiquidityUniverse();
            auto ranker = std::make_unique<IndicatorRanker>(
                IndicatorSpec{IndicatorKind::ROC, PriceField::Close, rankerRocLength},
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
        [](const ParameterValues&) { return true; }
    });

    definitions.push_back(StrategyDefinition{
        "PureMom",
        {
            {"heldBars", 7.0},
            {"rocLength", 7.0},
            {"btcMovingAverageLength", 50.0},
            {"quantityPercent", 10.0},
            {"maxPositionsOpen", 3.0},
            {"maxRankingPosition", static_cast<double>(std::numeric_limits<unsigned int>::max())}
        },
        makeSensitivityParameters({
            {"heldBars", "Held bars", 1.0, 14.0, 1.0, true},
            {"rocLength", "ROC length", 3.0, 15.0, 2.0, true},
            {"btcMovingAverageLength", "BTC moving-average length", 50.0, 50.0, 1.0, false},
            {"quantityPercent", "Quantity (%)", 2.0, 20.0, 2.0, false},
            {"maxPositionsOpen", "Maximum open positions", 1.0, 10.0, 1.0, false}
        }),
        [=](const ParameterValues& parameters) {
            const unsigned int heldBars = unsignedParameterOr(parameters, "heldBars", 7U);
            const unsigned int rocLength = unsignedParameterOr(parameters, "rocLength", 7U);
            const unsigned int btcMovingAverageLength = unsignedParameterOr(
                parameters, "btcMovingAverageLength", 50U
            );
            const double quantityPercent = parameterOr(parameters, "quantityPercent", 10.0);
            const unsigned int maxPositionsOpen = unsignedParameterOr(
                parameters, "maxPositionsOpen", 3U
            );
            const unsigned int maxRankingPosition = unsignedParameterOr(
                parameters,
                "maxRankingPosition",
                std::numeric_limits<unsigned int>::max()
            );

            auto universeSelector = makeTopLiquidityUniverse();
            auto ranker = std::make_unique<IndicatorRanker>(
                IndicatorSpec{IndicatorKind::ROC, PriceField::Close, rocLength},
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
        [](const ParameterValues&) { return true; }
    });

    definitions.push_back(StrategyDefinition{
        "PureRSI",
        {
            {"rsiLength", 7.0},
            {"rsiEntry", 80.0},
            {"rsiExit", 70.0},
            {"quantityPercent", 10.0},
            {"maxPositionsOpen", 10.0},
            {"maxRankingPosition", 1000000.0}
        },
        makeSensitivityParameters({
            {"rsiLength", "RSI length", 2.0, 20.0, 1.0, true},
            {"rsiEntry", "RSI entry", 60.0, 90.0, 5.0, true},
            {"rsiExit", "RSI exit", 40.0, 75.0, 5.0, true},
            {"quantityPercent", "Quantity (%)", 2.0, 20.0, 2.0, false},
            {"maxPositionsOpen", "Maximum open positions", 1.0, 20.0, 1.0, false}
        }),
        [=](const ParameterValues& parameters) {
            const unsigned int rsiLength = unsignedParameterOr(parameters, "rsiLength", 7U);
            const double rsiEntry = parameterOr(parameters, "rsiEntry", 80.0);
            const double rsiExit = parameterOr(parameters, "rsiExit", 70.0);
            const double quantityPercent = parameterOr(parameters, "quantityPercent", 10.0);
            const unsigned int maxPositionsOpen = unsignedParameterOr(
                parameters, "maxPositionsOpen", 10U
            );
            const unsigned int maxRankingPosition = unsignedParameterOr(
                parameters, "maxRankingPosition", 1000000U
            );

            auto universeSelector = makeTopLiquidityUniverse();
            auto ranker = std::make_unique<IndicatorRanker>(
                IndicatorSpec{IndicatorKind::RSI, PriceField::Close, rsiLength},
                true
            );

            return std::make_unique<StrategyPureRSI>(
                maxPositionsOpen,
                quantityPercent / 100.0,
                std::move(universeSelector),
                std::move(ranker),
                feeTaker,
                feeTaker,
                maxRankingPosition,
                rsiLength,
                rsiEntry,
                rsiExit
            );
        },
        [](const ParameterValues& parameters) {
            return parameterOr(parameters, "rsiEntry", 0.0) >
                   parameterOr(parameters, "rsiExit", 0.0);
        }
    });

    definitions.push_back(StrategyDefinition{
        "MRRSILong",
        {
            {"rsiLength", 3.0},
            {"rsiEntryLevel", 10.0},
            {"momentumLength", 30.0},
            {"heldBars", 1.0},
            {"quantityPercent", 10.0},
            {"maxPositionsOpen", 10.0},
            {"maxRankingPosition", static_cast<double>(std::numeric_limits<unsigned int>::max())}
        },
        makeSensitivityParameters({
            {"rsiLength", "RSI length", 2.0, 20.0, 2.0, true},
            {"rsiEntryLevel", "RSI entry level", 5.0, 35.0, 5.0, true},
            {"heldBars", "Held bars", 1.0, 20.0, 2.0, true},
            {"momentumLength", "Momentum length", 10.0, 60.0, 5.0, true},
            {"quantityPercent", "Quantity (%)", 2.0, 20.0, 2.0, false},
            {"maxPositionsOpen", "Maximum open positions", 1.0, 20.0, 1.0, false}
        }),
        [=](const ParameterValues& parameters) {
            const unsigned int rsiLength = unsignedParameterOr(parameters, "rsiLength", 3U);
            const double rsiEntryLevel = parameterOr(parameters, "rsiEntryLevel", 10.0);
            const unsigned int momentumLength = unsignedParameterOr(
                parameters, "momentumLength", 30U
            );
            const unsigned int heldBars = unsignedParameterOr(parameters, "heldBars", 1U);
            const double quantityPercent = parameterOr(parameters, "quantityPercent", 10.0);
            const unsigned int maxPositionsOpen = unsignedParameterOr(
                parameters, "maxPositionsOpen", 10U
            );
            const unsigned int maxRankingPosition = unsignedParameterOr(
                parameters,
                "maxRankingPosition",
                std::numeric_limits<unsigned int>::max()
            );

            auto universeSelector = makeTopLiquidityUniverse();
            auto ranker = std::make_unique<IndicatorRanker>(
                IndicatorSpec{IndicatorKind::ROC, PriceField::Close, momentumLength},
                true
            );

            return std::make_unique<StrategyMRRSILong>(
                maxPositionsOpen,
                quantityPercent / 100.0,
                std::move(universeSelector),
                std::move(ranker),
                feeMaker,
                feeTaker,
                maxRankingPosition,
                rsiLength,
                rsiEntryLevel,
                heldBars
            );
        },
        [](const ParameterValues&) { return true; }
    });

    definitions.push_back(StrategyDefinition{
        "XHBreakout",
        {
            {"xH", 50.0},
            {"fastMovingAverageLength", 5.0},
            {"momentumLength", 30.0},
            {"quantityPercent", 10.0},
            {"maxPositionsOpen", 10.0},
            {"maxRankingPosition", 9999999.0}
        },
        makeSensitivityParameters({
            {"xH", "XH lookback", 10.0, 100.0, 10.0, true},
            {"fastMovingAverageLength", "Fast moving-average length", 1.0, 15.0, 2.0, true},
            {"momentumLength", "Momentum length", 10.0, 60.0, 5.0, true},
            {"quantityPercent", "Quantity (%)", 2.0, 20.0, 2.0, false},
            {"maxPositionsOpen", "Maximum open positions", 1.0, 20.0, 1.0, false}
        }),
        [=](const ParameterValues& parameters) {
            const unsigned int xH = unsignedParameterOr(parameters, "xH", 50U);
            const unsigned int fastMovingAverageLength = unsignedParameterOr(
                parameters, "fastMovingAverageLength", 5U
            );
            const unsigned int momentumLength = unsignedParameterOr(
                parameters, "momentumLength", 30U
            );
            const double quantityPercent = parameterOr(parameters, "quantityPercent", 10.0);
            const unsigned int maxPositionsOpen = unsignedParameterOr(
                parameters, "maxPositionsOpen", 10U
            );
            const unsigned int maxRankingPosition = unsignedParameterOr(
                parameters, "maxRankingPosition", 9999999U
            );

            auto universeSelector = makeTopLiquidityUniverse();
            auto ranker = std::make_unique<IndicatorRanker>(
                IndicatorSpec{IndicatorKind::ROC, PriceField::Close, momentumLength},
                true
            );

            return std::make_unique<StrategyXHBreakout>(
                maxPositionsOpen,
                quantityPercent / 100.0,
                std::move(universeSelector),
                std::move(ranker),
                feeMaker,
                feeTaker,
                maxRankingPosition,
                xH,
                fastMovingAverageLength
            );
        },
        [](const ParameterValues&) { return true; }
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

std::vector<ParameterSensitivityReport> runParameterSensitivity(
    const StrategyDefinition& definition,
    const OHLCVData& ohlcvData,
    double initialBalance,
    double feeMaker,
    double feeTaker,
    const BacktestMetricsSettings& baseMetricsSettings,
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
            *progress
        );
    }

    (void)writeBacktestHtmlReport(
        reportPath,
        metrics,
        context.GetBalanceEquityHistoric(),
        context.GetMarketData(),
        parameterSensitivity
    );

    return metrics;
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

    const BacktestMetricsSettings metricsSettings{
        .periodsPerYear = 365.0,
        .excludeSimulatedTrades = true
    };

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

    std::size_t totalSensitivityRuns = 0U;
    if (runParameterSensitivity) {
        for (const StrategyDefinition& definition : strategyDefinitions) {
            const std::size_t strategyRuns = countSensitivityCombinations(definition);
            totalSensitivityRuns += strategyRuns;
            LG_INFO(
                "Sensitivity grid {}: {} valid isolated backtests",
                definition.name,
                strategyRuns
            );
        }
        LG_INFO(
            "Total parameter-sensitivity grid: {} isolated backtests",
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

    const std::filesystem::path reportsDirectory =
        "/mnt/c/Users/Juan/Documents/Python/algoTrading/storage/backtests/strategy_reports";

    for (const StrategyDefinition& definition : strategyDefinitions) {
        LG_INFO("============================================================");
        LG_INFO("Running isolated backtest for strategy: {}", definition.name);

        BacktestMetrics metrics = runStrategyAndWriteReport(
            definition,
            ohlcvData,
            initialBalance,
            feeMaker,
            feeTaker,
            metricsSettings,
            reportsDirectory / (definition.name + ".html"),
            progress.get()
        );

        if (progress) {
            std::cout << '\n';
        }
        logBacktestMetrics(metrics);
        metricsCache.insert_or_assign(definition.name, std::move(metrics));
    }

    if (progress) {
        progress->finish();
    }

    LG_INFO("============================================================");
    LG_INFO(
        "Completed {} isolated strategy backtests. Static HTML reports are in: {}",
        metricsCache.size(),
        reportsDirectory.string()
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
