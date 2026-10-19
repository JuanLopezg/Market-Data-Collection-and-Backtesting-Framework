#include <gtest/gtest.h>

#include "benchmark_above_sma_filter.h"
#include "indicator_ranker.h"
#include "liquidity_universe.h"
#include "strategy_test_data.h"

#include <memory>
#include <utility>
#include <vector>

namespace {
using namespace strategy_test;

constexpr IndicatorSpec quoteMean{IndicatorKind::SMA, PriceField::QuoteVolume, 2};
constexpr IndicatorSpec strength{IndicatorKind::RSI, PriceField::Close, 2};

std::vector<std::unique_ptr<MarketFilter>> benchmarkFilters()
{
    std::vector<std::unique_ptr<MarketFilter>> filters;
    filters.push_back(std::make_unique<BenchmarkAboveSMAFilter>("BENCH", 3));
    return filters;
}

TEST(PureRsi, R2_RsiStrictlyAboveEntryCreatesAFullLongOpinion)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 14, 13}); // RSI(2)=80 > 75.
    const auto strategy = makeRsi();
    SignalState signals;
    update(*strategy, raw, buildMarketData(raw), thirdClose, signals);
    EXPECT_DOUBLE_EQ(signals.get("A"), 1);
    EXPECT_EQ(signals.activeCount(), 1u);
}

TEST(PureRsi, R2_RsiEqualToEntryDoesNotEnter)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 13, 12}); // RSI(2)=75, exactly the entry threshold.
    const auto strategy = makeRsi();
    SignalState signals;
    update(*strategy, raw, buildMarketData(raw), thirdClose, signals);
    EXPECT_FALSE(signals.isActive("A"));
    EXPECT_TRUE(signals.values().empty());
}

TEST(PureRsi, R2_RsiBelowEntryDoesNotEnter)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 12, 11}); // RSI(2)=200/3 < 75.
    const auto strategy = makeRsi();
    SignalState signals;
    update(*strategy, raw, buildMarketData(raw), thirdClose, signals);
    EXPECT_TRUE(signals.values().empty());
}

TEST(PureRsi, R2_InsufficientPriceChangesCannotCreateAnEntry)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 14});
    const auto strategy = makeRsi();
    SignalState signals;
    update(*strategy, raw, buildMarketData(raw), 20240102, signals);
    EXPECT_TRUE(signals.values().empty());
}

TEST(PureRsi, R2_SevenBarRsiUsesTheConfiguredEntryAndExitThresholds)
{
    OHLCVData raw;
    addCloses(raw, "A", {100, 101, 102, 103, 104, 105, 106, 105, 102});
    const auto strategy = makeRsi({.length = 7, .entry = 80, .exit = 70});
    const auto market = buildMarketData(raw);
    SignalState signals;
    update(*strategy, raw, market, 20240107, signals);
    EXPECT_TRUE(signals.values().empty());
    update(*strategy, raw, market, 20240108, signals); // RSI=600/7 > 80.
    EXPECT_DOUBLE_EQ(signals.get("A"), 1);
    update(*strategy, raw, market, 20240109, signals); // RSI=400/7 < 70.
    EXPECT_TRUE(signals.values().empty());
}

TEST(PureRsi, R2_NewlyEligibleStrongAssetDoesNotNeedAFreshRsiCrossing)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 14, 13, 20});
    addCloses(raw, "OTHER", {10, 11, 10, 10});
    auto market = buildMarketData(raw);
    market.at(thirdClose).erase("A");
    const auto strategy = makeRsi();
    SignalState signals;
    update(*strategy, raw, market, thirdClose, signals);
    EXPECT_FALSE(signals.isActive("A"));
    update(*strategy, raw, market, 20240104, signals);
    EXPECT_DOUBLE_EQ(signals.get("A"), 1);
}

TEST(PureRsi, R3_RsiStrictlyBelowExitRemovesTheLongOpinion)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 11, 7}); // RSI(2)=20 < 25.
    const auto strategy = makeRsi();
    SignalState signals;
    signals.set("A", 1);
    update(*strategy, raw, buildMarketData(raw), thirdClose, signals);
    EXPECT_FALSE(signals.isActive("A"));
    EXPECT_TRUE(signals.values().empty());
}

TEST(PureRsi, R3_RsiEqualToExitRetainsTheExistingLong)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 11, 10}); // RSI(2)=50 exactly.
    const auto strategy = makeRsi({.exit = 50});
    SignalState signals;
    signals.set("A", 1);
    update(*strategy, raw, buildMarketData(raw), thirdClose, signals);
    EXPECT_DOUBLE_EQ(signals.get("A"), 1);
}

TEST(PureRsi, R3_UnavailableRsiOrMissingTimestampCannotLiquidateAnOpinion)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 14});
    const auto market = buildMarketData(raw);
    const auto strategy = makeRsi();
    SignalState signals;
    signals.set("A", 1);
    update(*strategy, raw, market, 20240102, signals); // RSI is still warming up.
    EXPECT_DOUBLE_EQ(signals.get("A"), 1);
    update(*strategy, raw, market, thirdClose, signals); // Entire timestamp missing.
    EXPECT_DOUBLE_EQ(signals.get("A"), 1);
    signals.set("UNKNOWN", 1);
    update(*strategy, raw, market, 20240102, signals);
    EXPECT_DOUBLE_EQ(signals.get("UNKNOWN"), 1);
}

TEST(PureRsi, R4_EntryHoldExitAndReentryFollowThePersistentOpinionLifecycle)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 14, 13, 12, 7, 16});
    const auto market = buildMarketData(raw);
    const auto strategy = makeRsi();
    SignalState signals;
    update(*strategy, raw, market, thirdClose, signals); // RSI 80.
    EXPECT_DOUBLE_EQ(signals.get("A"), 1);
    update(*strategy, raw, market, 20240104, signals); // RSI 400/7: below entry, above exit.
    EXPECT_DOUBLE_EQ(signals.get("A"), 1);
    update(*strategy, raw, market, 20240105, signals); // RSI 400/27.
    EXPECT_FALSE(signals.isActive("A"));
    update(*strategy, raw, market, 20240106, signals); // RSI 7600/99 > 75.
    EXPECT_DOUBLE_EQ(signals.get("A"), 1);
}

TEST(PureRsi, R4_ReevaluatingAnUnchangedCloseDoesNotDuplicateOpinions)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 14, 13});
    const auto strategy = makeRsi();
    const auto market = buildMarketData(raw);
    SignalState signals;
    update(*strategy, raw, market, thirdClose, signals);
    update(*strategy, raw, market, thirdClose, signals);
    EXPECT_EQ(signals.activeCount(), 1u);
    EXPECT_DOUBLE_EQ(signals.get("A"), 1);
}

TEST(PureRsi, R5_ExitFreesCapacityForANewSignalAtTheSameClose)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 11, 7});
    addCloses(raw, "B", {10, 14, 13});
    const auto strategy = makeRsi({.max_active_signals = 1});
    SignalState signals;
    signals.set("A", 1);
    update(*strategy, raw, buildMarketData(raw), thirdClose, signals);
    EXPECT_FALSE(signals.isActive("A"));
    EXPECT_DOUBLE_EQ(signals.get("B"), 1);
    EXPECT_EQ(signals.activeCount(), 1u);
}

TEST(PureRsi, R5_FailingBenchmarkCannotSuppressAnExistingSignalsExit)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 11, 7});
    addCloses(raw, "B", {10, 14, 13});
    addCloses(raw, "BENCH", {30, 20, 10});
    const auto strategy = makeRsi({.max_active_signals = 1}, {}, {}, benchmarkFilters());
    SignalState signals;
    signals.set("A", 1);
    update(*strategy, raw, buildMarketData(raw), thirdClose, signals);
    EXPECT_TRUE(signals.values().empty());
}

TEST(PureRsi, R5_HeldAssetOutsideEntryCrossSectionStillExitsFromItsRawHistory)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 11, 7});
    addCloses(raw, "B", {10, 14, 13});
    auto market = buildMarketData(raw);
    market.at(thirdClose).erase("A");
    const auto strategy = makeRsi({.max_active_signals = 1});
    SignalState signals;
    signals.set("A", 1);
    update(*strategy, raw, market, thirdClose, signals);
    EXPECT_FALSE(signals.isActive("A"));
    EXPECT_DOUBLE_EQ(signals.get("B"), 1);
}

TEST(PureRsi, R6_LiquidityRestrictsNewEntriesWithoutEvictingAnOldSignal)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 14, 13}, 100);
    addCloses(raw, "B", {20, 28, 26}, 400);
    const auto strategy = makeRsi({}, std::make_unique<TopNLiquidityUniverse>(quoteMean, 1));
    const auto market = buildMarketData(raw);
    SignalState fresh;
    update(*strategy, raw, market, thirdClose, fresh);
    EXPECT_FALSE(fresh.isActive("A"));
    EXPECT_DOUBLE_EQ(fresh.get("B"), 1);
    SignalState held;
    held.set("A", 1);
    update(*strategy, raw, market, thirdClose, held);
    EXPECT_DOUBLE_EQ(held.get("A"), 1);
    EXPECT_DOUBLE_EQ(held.get("B"), 1);
    EXPECT_EQ(held.activeCount(), 2u);
}

TEST(PureRsi, R6_MissingActualTurnoverCannotBeEstimatedIntoEntryEligibility)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 14, 13});
    const auto strategy = makeRsi({}, std::make_unique<TopNLiquidityUniverse>(quoteMean, 1));
    SignalState signals;
    update(*strategy, raw, buildMarketData(raw), thirdClose, signals);
    EXPECT_TRUE(signals.values().empty());
}

TEST(PureRsi, R7_NonqualifyingFirstRankDoesNotPromoteTheSecondRankIntoTheLimit)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 13, 12}); // RSI 75 does not qualify.
    addCloses(raw, "B", {10, 14, 13}); // RSI 80 qualifies, but is rank position 2.
    const auto strategy = makeRsi({.max_ranking_position = 1});
    SignalState signals;
    update(*strategy, raw, buildMarketData(raw), thirdClose, signals);
    EXPECT_TRUE(signals.values().empty());
}

TEST(PureRsi, R7_ConfiguredRsiRankingCanPromoteTheQualifyingAssetIntoTheLimit)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 13, 12});
    addCloses(raw, "B", {10, 14, 13});
    const auto strategy = makeRsi({.max_ranking_position = 1}, {},
                                   std::make_unique<IndicatorRanker>(strength));
    SignalState signals;
    update(*strategy, raw, buildMarketData(raw), thirdClose, signals);
    EXPECT_DOUBLE_EQ(signals.get("B"), 1);
    EXPECT_FALSE(signals.isActive("A"));
}

TEST(PureRsi, R7_AnAlreadyActiveFirstRankStillConsumesARankingPosition)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 14, 13});
    addCloses(raw, "B", {10, 14, 13});
    const auto strategy = makeRsi({.max_ranking_position = 1});
    SignalState signals;
    signals.set("A", 1);
    update(*strategy, raw, buildMarketData(raw), thirdClose, signals);
    EXPECT_DOUBLE_EQ(signals.get("A"), 1);
    EXPECT_FALSE(signals.isActive("B"));
}

TEST(PureRsi, R7_ZeroRankLimitBlocksNewEntriesButAllowsExits)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 11, 7});
    addCloses(raw, "B", {10, 14, 13});
    const auto strategy = makeRsi({.max_ranking_position = 0});
    SignalState signals;
    signals.set("A", 1);
    update(*strategy, raw, buildMarketData(raw), thirdClose, signals);
    EXPECT_TRUE(signals.values().empty());
}

TEST(PureRsi, R8_ActiveSignalCapSelectsOnlyTheFirstQualifyingCandidate)
{
    OHLCVData raw;
    addCloses(raw, "B", {10, 14, 13});
    addCloses(raw, "A", {10, 14, 13});
    const auto strategy = makeRsi({.max_active_signals = 1});
    SignalState signals;
    update(*strategy, raw, buildMarketData(raw), thirdClose, signals);
    EXPECT_EQ(signals.activeCount(), 1u);
    EXPECT_DOUBLE_EQ(signals.get("A"), 1);
    EXPECT_FALSE(signals.isActive("B"));
}

TEST(PureRsi, R8_StrongerNewcomerCannotEvictAStillValidExistingSignal)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 12, 11}); // Below entry, above exit.
    addCloses(raw, "B", {10, 11, 12}); // RSI 100.
    const auto strategy = makeRsi({.max_active_signals = 1}, {},
                                   std::make_unique<IndicatorRanker>(strength));
    SignalState signals;
    signals.set("A", 1);
    update(*strategy, raw, buildMarketData(raw), thirdClose, signals);
    EXPECT_DOUBLE_EQ(signals.get("A"), 1);
    EXPECT_FALSE(signals.isActive("B"));
}

TEST(PureRsi, R8_ZeroCapacityStillEvaluatesExistingExits)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 11, 7});
    addCloses(raw, "B", {10, 14, 13});
    const auto strategy = makeRsi({.max_active_signals = 0});
    SignalState signals;
    signals.set("A", 1);
    update(*strategy, raw, buildMarketData(raw), thirdClose, signals);
    EXPECT_TRUE(signals.values().empty());
}

TEST(PureRsi, R8_RestoredOpinionsAboveCapacityAreNotArbitrarilyDropped)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 14, 13});
    addCloses(raw, "B", {10, 14, 13});
    const auto strategy = makeRsi({.max_active_signals = 1});
    SignalState signals;
    signals.set("A", 1);
    signals.set("B", 1);
    update(*strategy, raw, buildMarketData(raw), thirdClose, signals);
    EXPECT_EQ(signals.activeCount(), 2u);
    EXPECT_DOUBLE_EQ(signals.get("A"), 1);
    EXPECT_DOUBLE_EQ(signals.get("B"), 1);
}

TEST(PureRsi, R9_AllBenchmarkFiltersMustPassBeforeNewEntry)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 14, 13});
    addCloses(raw, "BENCH", {10, 20, 30});
    addCloses(raw, "SECOND", {30, 20, 10});
    auto filters = benchmarkFilters();
    filters.push_back(std::make_unique<BenchmarkAboveSMAFilter>("SECOND", 3));
    const auto strategy = makeRsi({.max_active_signals = 1}, {}, {}, std::move(filters));
    SignalState signals;
    update(*strategy, raw, buildMarketData(raw), thirdClose, signals);
    EXPECT_TRUE(signals.values().empty());
    addCloses(raw, "SECOND", {10, 20, 30});
    update(*strategy, raw, buildMarketData(raw), thirdClose, signals);
    EXPECT_DOUBLE_EQ(signals.get("A"), 1);
}

TEST(PureRsi, R9_MissingOrWarmingBenchmarkBlocksNewEntry)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 14, 13});
    const auto strategy = makeRsi({}, {}, {}, benchmarkFilters());
    SignalState signals;
    update(*strategy, raw, buildMarketData(raw), thirdClose, signals);
    EXPECT_TRUE(signals.values().empty());
    raw.data["BENCH"][thirdClose] = {30, 33, 27, 30, 1};
    update(*strategy, raw, buildMarketData(raw), thirdClose, signals);
    EXPECT_TRUE(signals.values().empty());
}

TEST(PureRsi, R9_NullFilterSlotsDoNotBlockAValidEntry)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 14, 13});
    addCloses(raw, "BENCH", {10, 20, 30});
    auto filters = benchmarkFilters();
    filters.insert(filters.begin(), nullptr);
    const auto strategy = makeRsi({.max_active_signals = 1}, {}, {}, std::move(filters));
    SignalState signals;
    update(*strategy, raw, buildMarketData(raw), thirdClose, signals);
    EXPECT_DOUBLE_EQ(signals.get("A"), 1);
}

TEST(PureRsi, R9_FailingBenchmarkBlocksOnlyNewSignalsAndRetainsValidOldOpinion)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 12, 11});
    addCloses(raw, "B", {10, 14, 13});
    addCloses(raw, "BENCH", {30, 20, 10});
    const auto strategy = makeRsi({}, {}, {}, benchmarkFilters());
    SignalState signals;
    signals.set("A", 1);
    update(*strategy, raw, buildMarketData(raw), thirdClose, signals);
    EXPECT_DOUBLE_EQ(signals.get("A"), 1);
    EXPECT_FALSE(signals.isActive("B"));
    EXPECT_EQ(signals.activeCount(), 1u);
}

TEST(PureRsi, R10_DeclaredRequirementsSupportRealLiquidityRankingAndBenchmarkCollaborators)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 14, 13}, 100, 1000);
    addCloses(raw, "B", {20, 28, 26}, 400, 1);
    addCloses(raw, "BENCH", {10, 20, 30});
    const auto strategy = makeRsi({}, std::make_unique<TopNLiquidityUniverse>(quoteMean, 1),
        std::make_unique<IndicatorRanker>(strength), benchmarkFilters());
    SignalState signals;
    update(*strategy, raw, buildMarketData(raw), thirdClose, signals);
    EXPECT_DOUBLE_EQ(signals.get("B"), 1);
    EXPECT_FALSE(signals.isActive("A"));
    EXPECT_FALSE(signals.isActive("BENCH"));
    EXPECT_EQ(signals.activeCount(), 1u);
}

TEST(PureRsi, R10_FullLongOpinionsAreNotNormalizedMonetaryWeights)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 14, 13}, 100, 1000);
    addCloses(raw, "B", {100, 140, 130}, 400, 1);
    const auto strategy = makeRsi();
    SignalState signals;
    update(*strategy, raw, buildMarketData(raw), thirdClose, signals);
    EXPECT_DOUBLE_EQ(signals.get("A"), 1);
    EXPECT_DOUBLE_EQ(signals.get("B"), 1);
    EXPECT_DOUBLE_EQ(signals.get("A") + signals.get("B"), 2);
}

TEST(PureRsi, R15_FutureClosesAndNonSignalBarFieldsCannotRewriteAnEarlierEntry)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 14, 13});
    const auto strategy = makeRsi();
    SignalState original;
    update(*strategy, raw, buildMarketData(raw), thirdClose, original);
    auto changed = raw;
    for (auto& [date, bar] : changed.data.at("A")) {
        (void)date;
        bar.open = bar.close * 1.05;
        bar.high = bar.close * 1.2;
        bar.low = bar.close * 0.8;
        bar.volume = 1000000;
    }
    changed.data["A"][20240104] = {1, 1.1, 0.9, 1, 1000000};
    SignalState withFuture;
    update(*strategy, changed, buildMarketData(changed), thirdClose, withFuture);
    EXPECT_DOUBLE_EQ(original.get("A"), 1);
    EXPECT_DOUBLE_EQ(withFuture.get("A"), 1);
    EXPECT_EQ(withFuture.values(), original.values());
}

} // namespace
