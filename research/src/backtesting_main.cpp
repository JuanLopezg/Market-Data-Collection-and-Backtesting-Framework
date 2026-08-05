#include "backtest.h"
#include "all_strategies.h"
#include "backtest_helpers.h"
#include "benchmark_above_sma_filter.h"
#include "database_utils.h"
#include "indicator_ranker.h"
#include "indicator_spec.h"
#include "liquidity_universe.h"
#include "logger.h"
#include "market_filter.h"
#include "realtest.h"
#include "universe_selector.h"

#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

// Set exactly one switch to 1. All other switches must remain 0.
// Set exactly one switch to 1. Disabled strategy blocks are removed before compilation.
#define ENABLE_BARGAIN_CHASER 0
#define ENABLE_ATR_BREAKOUT 0
#define ENABLE_MR_SHORT 0
#define ENABLE_PURE_MOM 0
#define ENABLE_PURE_RSI 0
#define ENABLE_MR_RSI_LONG 0
#define ENABLE_XH_BREAKOUT 1
#define ENABLE_XH_BREAKOUT_ATR 0

std::string benchmark = "BTC";


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

    const std::filesystem::path backtestsDir =
        "/mnt/c/Users/Juan/Documents/Python/algoTrading/storage/backtests";

    // Set to true only when the reference CSV already exists.
    constexpr bool compareWithReferenceBacktest = false;

    LG_INFO("Database loading started");
    OHLCVData ohlcvData = loadDatabase(databasePath, 00000000);
    LG_INFO("Database loaded successfully");

    unsigned int lastTradeId = 0;
    double balance = 100000.0;
    double equity = balance;

    // Fees used by the backtest engine and by strategies that receive maker/taker fees.
    const double feeTaker = 0.0;
    const double feeMaker = 0.0;

    // Fees used by BargainChaser and ATRBreakout.
    const double commissionEntryFactor = 0.0;
    const double commissionExitFactor = 0.0;

    constexpr unsigned int topLiquidityCount = 20;

    auto makeTopLiquidityUniverse = [topLiquidityCount]() -> std::unique_ptr<UniverseSelector>
    {
        return std::make_unique<TopNLiquidityUniverse>(
            IndicatorSpec{
                IndicatorKind::SMA,
                PriceField::Volume,
                25
            },
            topLiquidityCount,
            true
        );
    };

    std::vector<std::unique_ptr<Strategy>> strategies;

    // Every enabled block sets the matching RealTest CSV and output paths.
    // Set exactly ONE ENABLE_* switch to 1, rebuild, and run.
    std::size_t enabledStrategyCount = 0;
    std::string activeStrategyName;
    std::filesystem::path tradesCsvPath;
    std::filesystem::path balanceEquityHtmlPath;
    std::filesystem::path realTestPath;
    std::filesystem::path mismatchesCsvPath;

    // ------------------------------------------------------------------
    // BargainChaser
    // ------------------------------------------------------------------
#if ENABLE_BARGAIN_CHASER
    {
            constexpr unsigned int maxPositionsOpen = 10;
            constexpr double riskPerTrade = 0.1;
            constexpr unsigned int maxRankingPosition = 999999;
            constexpr unsigned int barsUntilExit = 1;
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

            strategies.push_back(
                std::make_unique<StrategyBargainChaser>(
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
                )
            );

            activeStrategyName = "BargainChaser";
            tradesCsvPath = backtestsDir / "bargain_chaser_trades.csv";
            balanceEquityHtmlPath = backtestsDir / "bargain_chaser_balance_equity.html";
            realTestPath = backtestsDir / "final_tests/bargainChaser.csv";
            mismatchesCsvPath = backtestsDir / "bargain_chaser_mismatches.csv";
            ++enabledStrategyCount;
    }
#endif

    // ------------------------------------------------------------------
    // ATRBreakout
    // ------------------------------------------------------------------
#if ENABLE_ATR_BREAKOUT
    {
            constexpr unsigned int heldBars = 3;
            constexpr double atrMultiple = 0.875;
            constexpr unsigned int atrLength = 10;
            constexpr unsigned int momentumScoreNum = 30;
            constexpr double quantityPercent = 10.0;
            constexpr unsigned int maxPositionsOpen = 10;
            constexpr unsigned int maxRankingPosition = 999999;

            auto universeSelector = makeTopLiquidityUniverse();
            auto ranker = std::make_unique<IndicatorRanker>(
                IndicatorSpec{
                    IndicatorKind::ROC,
                    PriceField::Close,
                    momentumScoreNum
                },
                true
            );

            strategies.push_back(
                std::make_unique<StrategyATRBreakout>(
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
                )
            );

            activeStrategyName = "ATRBreakout";
            tradesCsvPath = backtestsDir / "atr_breakout_trades.csv";
            balanceEquityHtmlPath = backtestsDir / "atr_breakout_balance_equity.html";
            realTestPath = backtestsDir / "final_tests/atrBreakout.csv";
            mismatchesCsvPath = backtestsDir / "atr_breakout_mismatches.csv";
            ++enabledStrategyCount;
    }
#endif

    // ------------------------------------------------------------------
    // MRShort
    // ------------------------------------------------------------------
#if ENABLE_MR_SHORT
    {
            constexpr unsigned int rsiLength = 5;
            constexpr double rsiEntry = 70.0;
            constexpr unsigned int btcMovingAverageLength = 50;
            constexpr double entryAtrMultiple = 0.30;
            constexpr unsigned int entryAtrLength = 5;

            constexpr unsigned int heldBars = 7;
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

            strategies.push_back(
                std::make_unique<StrategyMRShort>(
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
                    benchmark
                )
            );

            activeStrategyName = "MRShort";
            tradesCsvPath = backtestsDir / "mr_short_trades.csv";
            balanceEquityHtmlPath = backtestsDir / "mr_short_balance_equity.html";
            realTestPath = backtestsDir / "final_tests/mrShort.csv";
            mismatchesCsvPath = backtestsDir / "mr_short_mismatches.csv";
            ++enabledStrategyCount;
    }
#endif

    // ------------------------------------------------------------------
    // PureMom
    // ------------------------------------------------------------------
#if ENABLE_PURE_MOM
    {
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

            strategies.push_back(
                std::make_unique<StrategyPureMom>(
                    maxPositionsOpen,
                    quantityPercent / 100.0,
                    std::move(universeSelector),
                    std::move(ranker),
                    feeTaker,
                    feeTaker,
                    maxRankingPosition,
                    heldBars,
                    btcMovingAverageLength,
                    benchmark
                )
            );

            activeStrategyName = "PureMom";
            tradesCsvPath = backtestsDir / "pure_mom_trades.csv";
            balanceEquityHtmlPath = backtestsDir / "pure_mom_balance_equity.html";
            realTestPath = backtestsDir / "final_tests/pureMom.csv";
            mismatchesCsvPath = backtestsDir / "pure_mom_mismatches.csv";
            ++enabledStrategyCount;
    }
#endif

    // ------------------------------------------------------------------
    // PureRSI
    // ------------------------------------------------------------------
#if ENABLE_PURE_RSI
    {
            constexpr unsigned int rsiLength = 7;
            constexpr double rsiEntry = 80.0;
            constexpr double rsiExit = 65.0;
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

            strategies.push_back(
                std::make_unique<StrategyPureRSI>(
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
                )
            );

            activeStrategyName = "PureRSI";
            tradesCsvPath = backtestsDir / "pure_rsi_trades.csv";
            balanceEquityHtmlPath = backtestsDir / "pure_rsi_balance_equity.html";
            realTestPath = backtestsDir / "final_tests/pureRSI.csv";
            mismatchesCsvPath = backtestsDir / "pure_rsi_mismatches.csv";
            ++enabledStrategyCount;
    }
#endif

    // ------------------------------------------------------------------
    // MRRSILong
    // ------------------------------------------------------------------
#if ENABLE_MR_RSI_LONG
    {
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

            strategies.push_back(
                std::make_unique<StrategyMRRSILong>(
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
                )
            );

            activeStrategyName = "MRRSILong";
            tradesCsvPath = backtestsDir / "mr_rsi_long_trades.csv";
            balanceEquityHtmlPath = backtestsDir / "mr_rsi_long_balance_equity.html";
            realTestPath = backtestsDir / "final_tests/mrRSILong.csv";
            mismatchesCsvPath = backtestsDir / "mr_rsi_long_mismatches.csv";
            ++enabledStrategyCount;
    }
#endif

    // ------------------------------------------------------------------
    // XHBreakout
    // ------------------------------------------------------------------
#if ENABLE_XH_BREAKOUT
    {
            constexpr unsigned int xH = 30;
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

            strategies.push_back(
                std::make_unique<StrategyXHBreakout>(
                    maxPositionsOpen,
                    quantityPercent / 100.0,
                    std::move(universeSelector),
                    std::move(ranker),
                    feeMaker,
                    feeTaker,
                    maxRankingPosition,
                    xH,
                    fastMovingAverageLength
                )
            );

            activeStrategyName = "XHBreakout";
            tradesCsvPath = backtestsDir / "xh_breakout_trades.csv";
            balanceEquityHtmlPath = backtestsDir / "xh_breakout_balance_equity.html";
            realTestPath = backtestsDir / "final_tests/xhBreakout.csv";
            mismatchesCsvPath = backtestsDir / "xh_breakout_mismatches.csv";
            ++enabledStrategyCount;
    }
#endif


    // ------------------------------------------------------------------
    // XHBreakout with ATR trailing stop
    // ------------------------------------------------------------------
#if ENABLE_XH_BREAKOUT_ATR
    {
            /*
             * Entry:
             *   Stop order at Highest(High, xH).
             *
             * Exit:
             *   close < highest high since entry
             *           - atrMultiplier * ATR(atrLength)
             */
            constexpr unsigned int xH = 30;
            constexpr unsigned int atrLength = 14;
            constexpr double atrMultiplier = 3.0;
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

            strategies.push_back(
                std::make_unique<StrategyXHBreakout_ATR>(
                    maxPositionsOpen,
                    quantityPercent / 100.0,
                    std::move(universeSelector),
                    std::move(ranker),
                    feeMaker,
                    feeTaker,
                    maxRankingPosition,
                    xH,
                    atrLength,
                    atrMultiplier
                )
            );

            activeStrategyName = "XHBreakout_ATR";
            tradesCsvPath = backtestsDir / "xh_breakout_atr_trades.csv";
            balanceEquityHtmlPath =
                backtestsDir / "xh_breakout_atr_balance_equity.html";
            realTestPath =
                backtestsDir / "final_tests/xhBreakoutATR.csv";
            mismatchesCsvPath =
                backtestsDir / "xh_breakout_atr_mismatches.csv";
            ++enabledStrategyCount;
    }
#endif

    if (enabledStrategyCount != 1)
    {
        LG_ERROR(
            "Enable exactly one strategy. Enabled strategy count: {}",
            enabledStrategyCount
        );
        return 1;
    }

    LG_INFO("Active strategy: {}", activeStrategyName);

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

    const MarketData& marketData = context.GetMarketData();
    LG_INFO("Market data and indicators initialized");

    Backtester tester(context);

    LG_INFO("Starting loop");
    tester.loop();
    LG_INFO("Finished loop");

    tester.closeTrades();

    tester.storeTradesCSV(tradesCsvPath);

    printBalanceEquityChart(
        context.GetBalanceEquityHistoric(),
        marketData,
        balanceEquityHtmlPath.string()
    );

    if (compareWithReferenceBacktest)
    {
        compareBacktests(
            realTestPath,
            context.GetTradesHistory(),
            false,
            mismatchesCsvPath
        );
    }
    else
    {
        LG_INFO(
            "Reference comparison disabled. Set compareWithReferenceBacktest=true "
            "when the reference CSV exists: {}",
            realTestPath.string()
        );
    }

    return 0;
}
