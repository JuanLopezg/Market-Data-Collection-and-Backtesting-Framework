#include "backtest.h"
#include "all_strategies.h"
#include "backtest_metrics.h"
#include "database_utils.h"
#include "indicator_ranker.h"
#include "indicator_spec.h"
#include "liquidity_universe.h"
#include "logger.h"
#include "universe_selector.h"

#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

struct StrategyDefinition {
    std::string name;
    std::function<std::unique_ptr<Strategy>()> create;
};

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

std::vector<StrategyDefinition> makeStrategyDefinitions(
    double feeMaker,
    double feeTaker,
    double commissionEntryFactor,
    double commissionExitFactor
)
{
    std::vector<StrategyDefinition> definitions;

    definitions.push_back({
        "BargainChaser",
        [=]() {
            constexpr unsigned int maxPositionsOpen = 10;
            constexpr double riskPerTrade = 0.1;
            constexpr unsigned int maxRankingPosition = 999999;
            constexpr unsigned int barsUntilExit = 2;
            constexpr double fallPercentage = 10.0;
            constexpr unsigned int movingAverageLength = 50;

            auto universeSelector = makeTopLiquidityUniverse();

            auto ranker = std::make_unique<IndicatorRanker>(
                IndicatorSpec{
                    IndicatorKind::ROC,
                    PriceField::Close,
                    1
                },
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
        }
    });

    definitions.push_back({
        "ATRBreakout",
        [=]() {
            constexpr unsigned int heldBars = 2;
            constexpr double atrMultiple = 0.75;
            constexpr unsigned int atrLength = 3;
            constexpr unsigned int momentumScoreLength = 30;
            constexpr double quantityPercent = 10.0;
            constexpr unsigned int maxPositionsOpen = 10;
            constexpr unsigned int maxRankingPosition = 999999;

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
        }
    });

    definitions.push_back({
        "MRShort",
        [=]() {
            constexpr unsigned int rsiLength = 5;
            constexpr double rsiEntry = 70.0;
            constexpr unsigned int btcMovingAverageLength = 50;
            constexpr double entryAtrMultiple = 0.30;
            constexpr unsigned int entryAtrLength = 5;
            constexpr unsigned int heldBars = 3;
            constexpr double quantityPercent = 10.0;
            constexpr unsigned int maxPositionsOpen = 10;
            constexpr unsigned int maxRankingPosition = 1000000;

            auto universeSelector = makeTopLiquidityUniverse();

            auto ranker = std::make_unique<IndicatorRanker>(
                IndicatorSpec{
                    IndicatorKind::ROC,
                    PriceField::Close,
                    30
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
        }
    });

    definitions.push_back({
        "PureMom",
        [=]() {
            constexpr unsigned int heldBars = 7;
            constexpr unsigned int rocLength = 7;
            constexpr unsigned int btcMovingAverageLength = 50;
            constexpr double quantityPercent = 10.0;
            constexpr unsigned int maxPositionsOpen = 3;

            constexpr unsigned int maxRankingPosition =
                std::numeric_limits<unsigned int>::max();

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
        }
    });

    definitions.push_back({
        "PureRSI",
        [=]() {
            constexpr unsigned int rsiLength = 7;
            constexpr double rsiEntry = 80.0;
            constexpr double rsiExit = 70.0;
            constexpr double quantityPercent = 10.0;
            constexpr unsigned int maxPositionsOpen = 10;
            constexpr unsigned int maxRankingPosition = 1000000;

            auto universeSelector = makeTopLiquidityUniverse();

            auto ranker = std::make_unique<IndicatorRanker>(
                IndicatorSpec{
                    IndicatorKind::RSI,
                    PriceField::Close,
                    rsiLength
                },
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
        }
    });

    definitions.push_back({
        "MRRSILong",
        [=]() {
            constexpr unsigned int rsiLength = 3;
            constexpr double rsiEntryLevel = 10.0;
            constexpr unsigned int momentumLength = 30;
            constexpr unsigned int heldBars = 1;
            constexpr double quantityPercent = 10.0;
            constexpr unsigned int maxPositionsOpen = 10;

            constexpr unsigned int maxRankingPosition =
                std::numeric_limits<unsigned int>::max();

            auto universeSelector = makeTopLiquidityUniverse();

            auto ranker = std::make_unique<IndicatorRanker>(
                IndicatorSpec{
                    IndicatorKind::ROC,
                    PriceField::Close,
                    momentumLength
                },
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
        }
    });

    definitions.push_back({
        "XHBreakout",
        [=]() {
            constexpr unsigned int xH = 50;
            constexpr unsigned int fastMovingAverageLength = 5;
            constexpr unsigned int momentumLength = 30;
            constexpr double quantityPercent = 10.0;
            constexpr unsigned int maxPositionsOpen = 10;
            constexpr unsigned int maxRankingPosition = 9999999;

            auto universeSelector = makeTopLiquidityUniverse();

            auto ranker = std::make_unique<IndicatorRanker>(
                IndicatorSpec{
                    IndicatorKind::ROC,
                    PriceField::Close,
                    momentumLength
                },
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
        }
    });

    return definitions;
}

BacktestMetrics runStrategyAndCalculateMetrics(
    const StrategyDefinition& definition,
    const OHLCVData& ohlcvData,
    double initialBalance,
    double feeMaker,
    double feeTaker,
    const BacktestMetricsSettings& metricsSettings
)
{
    unsigned int lastTradeId = 0;
    double balance = initialBalance;
    double equity = initialBalance;

    std::vector<std::unique_ptr<Strategy>> strategies;
    strategies.push_back(definition.create());

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

} // namespace

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    Logger::Instance().Setup(
        true,
        false,
        "",
        "",
        true
    );

    const std::string databasePath =
        "/mnt/c/Users/Juan/Documents/Python/algoTrading/storage/databases/1d_cmc.csv";

    constexpr double initialBalance = 100000.0;

    // Mantener estos valores iguales a los usados para validar contra RealTest.
    constexpr double feeTaker = 0.0;
    constexpr double feeMaker = 0.0;
    constexpr double commissionEntryFactor = 0.0;
    constexpr double commissionExitFactor = 0.0;

    const BacktestMetricsSettings metricsSettings{
        .periodsPerYear = 365.0,
        .excludeSimulatedTrades = true
    };

    LG_INFO("Database loading started");

    OHLCVData ohlcvData = loadDatabase(
        databasePath,
        00000000
    );

    LG_INFO("Database loaded successfully");

    const std::vector<StrategyDefinition> strategyDefinitions =
        makeStrategyDefinitions(
            feeMaker,
            feeTaker,
            commissionEntryFactor,
            commissionExitFactor
        );

    // Solo existe durante la ejecución. No se escribe a disco.
    std::map<std::string, BacktestMetrics> metricsCache;

    for (const StrategyDefinition& definition : strategyDefinitions) {
        LG_INFO("============================================================");
        LG_INFO("Running isolated backtest for strategy: {}", definition.name);

        BacktestMetrics metrics = runStrategyAndCalculateMetrics(
            definition,
            ohlcvData,
            initialBalance,
            feeMaker,
            feeTaker,
            metricsSettings
        );

        logBacktestMetrics(metrics);

        metricsCache.insert_or_assign(
            definition.name,
            std::move(metrics)
        );
    }

    LG_INFO("============================================================");

    LG_INFO(
        "Completed {} isolated strategy backtests. Metrics remain in memory only.",
        metricsCache.size()
    );

    for (const auto& [strategyName, metrics] : metricsCache) {
        LG_INFO(
            "Summary {}: return={:.2f}% annualized={:.2f}% "
            "max_dd={:.2f}% sharpe={:.3f} sortino={:.3f} "
            "calmar={:.3f} profit_factor={:.3f} trades={}",
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