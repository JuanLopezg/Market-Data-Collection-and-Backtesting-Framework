#pragma once

#include "timed_research_strategies.h"
#include "xh_breakout_strategy.h"
#include "validated/pure_rsi.h"
#include "indicator_ranker.h"
#include "liquidity_universe.h"
#include "equal_weight_sizer.h"
#include "entry_exit_only_rebalance_policy.h"
#include "strategy_instance.h"

#include <functional>
#include <limits>
#include <map>

// Original seven-strategy configurations and parameter ranges. Factories construct
// signals; the shared portfolio configuration keeps sizing separate from signals.
namespace research_studies {

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

inline double parameterOr(
    const ParameterValues& parameters,
    const std::string& key,
    double fallback
)
{
    const auto iterator = parameters.find(key);
    return iterator == parameters.end() ? fallback : iterator->second;
}

inline unsigned int unsignedParameterOr(
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

inline std::unique_ptr<UniverseSelector> makeTopLiquidityUniverse()
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

inline std::vector<SensitivityParameterDefinition> makeSensitivityParameters(
    std::initializer_list<SensitivityParameterDefinition> parameters
)
{
    return std::vector<SensitivityParameterDefinition>(

        parameters.begin(),
        parameters.end()
    );
}

inline std::vector<StrategyDefinition> makeStrategyDefinitions()
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
        [](const ParameterValues& parameters) {
            const unsigned int maxPositionsOpen = unsignedParameterOr(
                parameters, "maxPositionsOpen", 10U
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

            return std::make_unique<BargainChaserStrategy>(
                maxPositionsOpen,
                std::move(universeSelector),
                std::move(ranker),
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
        [](const ParameterValues& parameters) {
            const unsigned int heldBars = unsignedParameterOr(parameters, "heldBars", 2U);
            const double atrMultiple = parameterOr(parameters, "atrMultiple", 0.75);
            const unsigned int atrLength = unsignedParameterOr(parameters, "atrLength", 3U);
            const unsigned int momentumScoreLength = unsignedParameterOr(
                parameters, "momentumScoreLength", 30U
            );
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

            return std::make_unique<ATRBreakoutStrategy>(
                maxPositionsOpen,
                std::move(universeSelector),
                std::move(ranker),
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
        [](const ParameterValues& parameters) {
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

            return std::make_unique<ShortMeanReversionStrategy>(
                maxPositionsOpen,
                std::move(universeSelector),
                std::move(ranker),
                maxRankingPosition,
                rsiLength,
                rsiEntry,
                btcMovingAverageLength,
                entryAtrMultiple,
                entryAtrLength,
                heldBars
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
        [](const ParameterValues& parameters) {
            const unsigned int heldBars = unsignedParameterOr(parameters, "heldBars", 7U);
            const unsigned int rocLength = unsignedParameterOr(parameters, "rocLength", 7U);
            const unsigned int btcMovingAverageLength = unsignedParameterOr(
                parameters, "btcMovingAverageLength", 50U
            );
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

            return std::make_unique<PureMomentumStrategy>(
                maxPositionsOpen,
                std::move(universeSelector),
                std::move(ranker),
                maxRankingPosition,
                heldBars,
                btcMovingAverageLength
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
        [](const ParameterValues& parameters) {
            const unsigned int rsiLength = unsignedParameterOr(parameters, "rsiLength", 7U);
            const double rsiEntry = parameterOr(parameters, "rsiEntry", 80.0);
            const double rsiExit = parameterOr(parameters, "rsiExit", 70.0);
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
                std::move(universeSelector),
                std::move(ranker),
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
        [](const ParameterValues& parameters) {
            const unsigned int rsiLength = unsignedParameterOr(parameters, "rsiLength", 3U);
            const double rsiEntryLevel = parameterOr(parameters, "rsiEntryLevel", 10.0);
            const unsigned int momentumLength = unsignedParameterOr(
                parameters, "momentumLength", 30U
            );
            const unsigned int heldBars = unsignedParameterOr(parameters, "heldBars", 1U);
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

            return std::make_unique<RSIMeanReversionStrategy>(
                maxPositionsOpen,
                std::move(universeSelector),
                std::move(ranker),
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
        [](const ParameterValues& parameters) {
            const unsigned int xH = unsignedParameterOr(parameters, "xH", 50U);
            const unsigned int fastMovingAverageLength = unsignedParameterOr(
                parameters, "fastMovingAverageLength", 5U
            );
            const unsigned int momentumLength = unsignedParameterOr(
                parameters, "momentumLength", 30U
            );
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

            return std::make_unique<XHBreakoutStrategy>(
                maxPositionsOpen,
                std::move(universeSelector),
                std::move(ranker),
                maxRankingPosition,
                xH,
                fastMovingAverageLength
            );
        },
        [](const ParameterValues&) { return true; }
    });

    return definitions;
}


inline StrategyPortfolio makeStudyPortfolio(const StrategyDefinition& definition, const ParameterValues& parameters)
{
    const double assetWeight = definition.name == "BargainChaser"
        ? parameterOr(parameters, "riskPerTrade", 0.1)
        : parameterOr(parameters, "quantityPercent", 10.0) / 100.0;
    StrategyPortfolio portfolio;
    portfolio.emplace_back(1, definition.create(parameters), 1.0,
        std::make_unique<EqualWeightSizer>(assetWeight), RiskConstraints(1.5, 1.5),
        std::make_unique<EntryExitOnlyRebalancePolicy>());
    return portfolio;
}

} // namespace research_studies
