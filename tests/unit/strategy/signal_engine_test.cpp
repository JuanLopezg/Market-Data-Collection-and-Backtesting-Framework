#include <gtest/gtest.h>

#include "rolling_market_state.h"
#include "strategy_test_data.h"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
using namespace strategy_test;

using Opinions = std::unordered_map<Coin, double>;

OHLCVData completedHistory(const OHLCVData& raw, Timestamp date)
{
    auto result = raw;
    for (auto& [coin, series] : result.data) {
        (void)coin;
        series.erase(series.upper_bound(date), series.end());
    }
    return result;
}

TEST(SignalOpinions, R1_ClosingOneOpinionPreservesIndependentShortAndFractionalOpinions)
{
    SignalState signals;
    signals.set("A", 1);
    signals.set("B", -0.25);
    signals.set("C", 0.5);
    signals.set("A", 0);
    signals.set("A", 0);
    EXPECT_EQ(signals.values(), (Opinions{{"B", -0.25}, {"C", 0.5}}));
    EXPECT_EQ(signals.activeCount(), 2u);
    EXPECT_FALSE(signals.isActive("A"));
    EXPECT_DOUBLE_EQ(signals.get("A"), 0);
}

TEST(SignalOpinions, R1_InvalidOpinionsCannotReplaceAnAcceptedValueOrAddAnAsset)
{
    SignalState signals;
    signals.set("A", 1);
    for (double invalid : {1.01, -1.01, std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity()}) {
        EXPECT_THROW(signals.set("A", invalid), std::out_of_range);
        EXPECT_THROW(signals.set("B", invalid), std::out_of_range);
        EXPECT_EQ(signals.values(), (Opinions{{"A", 1}}));
    }
}

TEST(SignalOpinions, R1_RepeatedOpinionsCannotAccumulateExposureOrActiveSlots)
{
    SignalState signals;
    for (unsigned delivery = 0; delivery < 3; ++delivery) {
        signals.set("A", 1);
        signals.set("B", -0.25);
    }
    EXPECT_EQ(signals.activeCount(), 2u);
    EXPECT_EQ(signals.values(), (Opinions{{"A", 1}, {"B", -0.25}}));
}

TEST(SignalOpinions, R1_RestoringCompleteSignalsReplacesOldOpinionsAndRemovesZeros)
{
    StrategySignalInstance instance(7, makeRsi());
    instance.signalState().set("A", 1);
    instance.restoreSignals({{"A", 0}, {"B", 0.5}, {"C", -0.25}});
    EXPECT_EQ(instance.signalState().values(), (Opinions{{"B", 0.5}, {"C", -0.25}}));
    instance.restoreSignals({});
    EXPECT_TRUE(instance.signalState().values().empty());
}

TEST(SignalOpinions, R1_InvalidInstanceRestorePreservesTheEntirePreviousState)
{
    StrategySignalInstance instance(7, makeRsi());
    instance.signalState().set("A", 1);
    EXPECT_THROW(instance.restoreSignals({{"B", 0.5}, {"C", 2}}), std::out_of_range);
    EXPECT_EQ(instance.signalState().values(), (Opinions{{"A", 1}}));
}

TEST(StrategyIntents, R12_WarmupStillEmitsOneCompleteEmptyRecordPerConfiguredStrategy)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 14});
    auto engine = twoStrategyEngine();
    const auto batch = engine.onBarClose(raw, buildMarketData(raw), 20240102);
    EXPECT_EQ(batch.timestamp, 20240102u);
    ASSERT_EQ(batch.strategies.size(), 2u);
    EXPECT_TRUE(intent(batch, 7).signals.empty());
    EXPECT_TRUE(intent(batch, 9).signals.empty());
    EXPECT_EQ(intent(batch, 7).strategy_name, "Pure_RSI");
    EXPECT_EQ(intent(batch, 9).strategy_name, "Pure_RSI");
}

TEST(StrategyIntents, R12_StrategyIdsKeepIndependentlyConfiguredThresholdsSeparate)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 14, 13});
    auto engine = twoStrategyEngine();
    const auto batch = engine.onBarClose(raw, buildMarketData(raw), thirdClose);
    ASSERT_EQ(batch.strategies.size(), 2u);
    EXPECT_EQ(batch.timestamp, thirdClose);
    EXPECT_EQ(intent(batch, 7).signals, (Opinions{{"A", 1}}));
    EXPECT_TRUE(intent(batch, 9).signals.empty()); // RSI 80 is below ID 9's entry=85.
}

TEST(StrategyIntents, R12_UnchangedHoldIsIncludedInTheNextFullSnapshot)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 14, 13, 12});
    auto engine = singleEngine();
    const auto prefix = completedHistory(raw, thirdClose);
    const auto entry = engine.onBarClose(prefix, buildMarketData(prefix), thirdClose);
    const auto held = engine.onBarClose(raw, buildMarketData(raw), 20240104);
    EXPECT_EQ(intent(entry, 7).signals, (Opinions{{"A", 1}}));
    EXPECT_EQ(intent(held, 7).signals, (Opinions{{"A", 1}}));
    EXPECT_EQ(held.timestamp, 20240104u);
}

TEST(StrategyIntents, R12_LaterExitCannotRewriteAnAlreadyReturnedEntrySnapshot)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 14, 13, 12, 7});
    auto engine = singleEngine();
    const auto prefix = completedHistory(raw, thirdClose);
    const auto entry = engine.onBarClose(prefix, buildMarketData(prefix), thirdClose);
    const auto exited = engine.onBarClose(raw, buildMarketData(raw), 20240105);
    EXPECT_TRUE(intent(exited, 7).signals.empty());
    EXPECT_EQ(intent(entry, 7).signals, (Opinions{{"A", 1}}));
    EXPECT_EQ(entry.timestamp, thirdClose);
}

TEST(StrategyIntents, R11_DuplicateAndOlderClosesCannotRepeatOrRewindSignals)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 14, 13});
    const auto market = buildMarketData(raw);
    auto engine = singleEngine();
    const auto accepted = engine.onBarClose(raw, market, thirdClose);
    EXPECT_THROW(engine.onBarClose(raw, market, thirdClose), std::logic_error);
    EXPECT_THROW(engine.onBarClose(raw, market, 20240102), std::logic_error);
    EXPECT_EQ(engine.lastTimestamp(), thirdClose);
    EXPECT_EQ(engine.strategies().at(0).signalState().values(), intent(accepted, 7).signals);
}

TEST(StrategyIntents, R11_ZeroOrUnavailableCloseCannotAdvanceTheFrontier)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 14, 13, 12});
    const auto prefix = completedHistory(raw, thirdClose);
    auto engine = singleEngine();
    const auto accepted = engine.onBarClose(prefix, buildMarketData(prefix), thirdClose);
    EXPECT_THROW(engine.onBarClose(prefix, buildMarketData(prefix), 0), std::invalid_argument);
    EXPECT_THROW(engine.onBarClose(prefix, buildMarketData(prefix), 20240104), std::out_of_range);
    EXPECT_EQ(engine.lastTimestamp(), thirdClose);
    EXPECT_EQ(engine.strategies().at(0).signalState().values(), intent(accepted, 7).signals);
    const auto next = engine.onBarClose(raw, buildMarketData(raw), 20240104);
    EXPECT_EQ(intent(next, 7).signals, (Opinions{{"A", 1}}));
}

TEST(StrategyIntents, R13_InvalidPortfolioOwnershipOrDuplicateIdsAreRejected)
{
    EXPECT_THROW((StrategySignalInstance{7, nullptr}), std::invalid_argument);
    EXPECT_THROW((StrategySignalEngine{StrategySignalPortfolio{}}), std::invalid_argument);
    StrategySignalPortfolio duplicate;
    duplicate.emplace_back(7, makeRsi());
    duplicate.emplace_back(7, makeRsi());
    EXPECT_THROW((StrategySignalEngine{std::move(duplicate)}), std::invalid_argument);
}

TEST(StrategyIntents, R14_RestoredOpinionDoesNotNeedToQualifyForEntryAgain)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 13, 12, 12}); // RSI 75: never strictly above entry.
    auto engine = singleEngine();
    engine.restore(checkpoint(thirdClose, {{7, "Pure_RSI", {{"A", 1}}}}));
    const auto held = engine.onBarClose(raw, buildMarketData(raw), 20240104);
    EXPECT_EQ(intent(held, 7).signals, (Opinions{{"A", 1}}));
}

TEST(StrategyIntents, R14_RestartedLifecycleMatchesUninterruptedHoldExitAndReentry)
{
    OHLCVData full;
    addCloses(full, "A", {10, 14, 13, 12, 7, 16});
    auto uninterrupted = singleEngine();
    const auto prefix = completedHistory(full, thirdClose);
    const auto saved = uninterrupted.onBarClose(prefix, buildMarketData(prefix), thirdClose);
    auto restarted = singleEngine();
    restarted.restore(saved);
    const std::vector<std::pair<Timestamp, double>> expected{
        {20240104, 1}, {20240105, 0}, {20240106, 1}};
    for (const auto& [date, opinion] : expected) {
        const auto raw = completedHistory(full, date);
        const auto market = buildMarketData(raw);
        const auto continuous = uninterrupted.onBarClose(raw, market, date);
        const auto resumed = restarted.onBarClose(raw, market, date);
        EXPECT_EQ(intent(resumed, 7).signals, intent(continuous, 7).signals);
        if (opinion == 0) EXPECT_TRUE(intent(resumed, 7).signals.empty());
        else EXPECT_EQ(intent(resumed, 7).signals, (Opinions{{"A", opinion}}));
        EXPECT_EQ(restarted.lastTimestamp(), date);
    }
}

TEST(StrategyIntents, R14_CheckpointDateCannotBeProcessedAgainAfterRestart)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 14, 13});
    auto engine = singleEngine();
    engine.restore(checkpoint(thirdClose, {{7, "Pure_RSI", {{"A", 1}}}}));
    EXPECT_THROW(engine.onBarClose(raw, buildMarketData(raw), thirdClose), std::logic_error);
    EXPECT_EQ(engine.lastTimestamp(), thirdClose);
    EXPECT_EQ(engine.strategies().at(0).signalState().values(), (Opinions{{"A", 1}}));
}

TEST(StrategyIntents, R14_CheckpointRecordsAreMatchedByIdRatherThanVectorOrder)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 14, 13, 12});
    addCloses(raw, "B", {10, 14, 13, 12});
    auto engine = twoStrategyEngine();
    engine.restore(checkpoint(thirdClose,
        {{9, "Pure_RSI", {{"B", 1}}}, {7, "Pure_RSI", {{"A", 1}}}}));
    const auto batch = engine.onBarClose(raw, buildMarketData(raw), 20240104);
    EXPECT_EQ(intent(batch, 7).signals, (Opinions{{"A", 1}}));
    EXPECT_EQ(intent(batch, 9).signals, (Opinions{{"B", 1}}));
}

TEST(StrategyIntents, R14_CompleteCheckpointReplacesSignalsInsteadOfMergingOldOpinions)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 14, 13, 12, 11});
    auto engine = singleEngine();
    const auto prefix = completedHistory(raw, thirdClose);
    engine.onBarClose(prefix, buildMarketData(prefix), thirdClose);
    engine.restore(checkpoint(20240104, {{7, "Pure_RSI", {}}}));
    const auto next = engine.onBarClose(raw, buildMarketData(raw), 20240105);
    // RSI is between exit and entry here: only a stale merged opinion would stay long.
    EXPECT_TRUE(intent(next, 7).signals.empty());
}

TEST(StrategyIntents, R14_RepeatingTheSameCheckpointIsIdempotent)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 14, 13, 12});
    const auto saved = checkpoint(thirdClose, {{7, "Pure_RSI", {{"A", 1}}}});
    auto engine = singleEngine();
    engine.restore(saved);
    engine.restore(saved);
    EXPECT_EQ(engine.lastTimestamp(), thirdClose);
    const auto next = engine.onBarClose(raw, buildMarketData(raw), 20240104);
    EXPECT_EQ(intent(next, 7).signals, (Opinions{{"A", 1}}));
}

TEST(StrategyIntents, R13_InvalidCheckpointShapeOrIdentityCannotReplaceAcceptedState)
{
    const auto saved = checkpoint(thirdClose,
        {{7, "Pure_RSI", {{"A", 1}}}, {9, "Pure_RSI", {{"B", 1}}}});
    const std::vector<StrategyIntentBatch> invalid{
        checkpoint(0, {{7, "Pure_RSI", {}}, {9, "Pure_RSI", {}}}),
        checkpoint(20240104, {}),
        checkpoint(20240104, {{7, "Pure_RSI", {}}}),
        checkpoint(20240104, {{7, "Pure_RSI", {}}, {9, "Pure_RSI", {}}, {11, "Pure_RSI", {}}}),
        checkpoint(20240104, {{7, "Pure_RSI", {}}, {7, "Pure_RSI", {}}}),
        checkpoint(20240104, {{8, "Pure_RSI", {}}, {9, "Pure_RSI", {}}}),
        checkpoint(20240104, {{7, "Renamed", {}}, {9, "Pure_RSI", {}}})
    };
    auto engine = twoStrategyEngine();
    for (const auto& bad : invalid) {
        engine.restore(saved);
        EXPECT_THROW(engine.restore(bad), std::invalid_argument);
        EXPECT_EQ(engine.lastTimestamp(), thirdClose);
        EXPECT_EQ(engine.strategies().at(0).signalState().values(), (Opinions{{"A", 1}}));
        EXPECT_EQ(engine.strategies().at(1).signalState().values(), (Opinions{{"B", 1}}));
    }
}

TEST(StrategyIntents, R13_InvalidRestoredOpinionCannotChangeSignalsOrFrontier)
{
    auto engine = singleEngine();
    const auto saved = checkpoint(thirdClose, {{7, "Pure_RSI", {{"A", 1}}}});
    for (double invalid : {2.0, -1.01, std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity()}) {
        engine.restore(saved);
        EXPECT_THROW(engine.restore(checkpoint(20240104, {{7, "Pure_RSI", {{"B", invalid}}}})),
                     std::out_of_range);
        EXPECT_EQ(engine.lastTimestamp(), thirdClose);
        EXPECT_EQ(engine.strategies().at(0).signalState().values(), (Opinions{{"A", 1}}));
    }
}

TEST(StrategyIntents, R14_LaterStrategyNameErrorCannotPartiallyApplyACheckpoint)
{
    auto engine = twoStrategyEngine();
    engine.restore(checkpoint(thirdClose,
        {{7, "Pure_RSI", {{"A", 1}}}, {9, "Pure_RSI", {{"B", 1}}}}));
    const auto invalid = checkpoint(20240104,
        {{7, "Pure_RSI", {{"C", 1}}}, {9, "Renamed", {{"B", 1}}}});
    EXPECT_THROW(engine.restore(invalid), std::invalid_argument);
    EXPECT_EQ(engine.lastTimestamp(), thirdClose);
    EXPECT_EQ(engine.strategies().at(0).signalState().values(), (Opinions{{"A", 1}}));
    EXPECT_EQ(engine.strategies().at(1).signalState().values(), (Opinions{{"B", 1}}));
}

TEST(StrategyIntents, R14_LaterStrategySignalErrorCannotPartiallyApplyACheckpoint)
{
    auto engine = twoStrategyEngine();
    engine.restore(checkpoint(thirdClose,
        {{7, "Pure_RSI", {{"A", 1}}}, {9, "Pure_RSI", {{"B", 1}}}}));
    const auto invalid = checkpoint(20240104,
        {{7, "Pure_RSI", {{"C", 1}}}, {9, "Pure_RSI", {{"B", 2}}}});
    EXPECT_THROW(engine.restore(invalid), std::out_of_range);
    EXPECT_EQ(engine.lastTimestamp(), thirdClose);
    EXPECT_EQ(engine.strategies().at(0).signalState().values(), (Opinions{{"A", 1}}));
    EXPECT_EQ(engine.strategies().at(1).signalState().values(), (Opinions{{"B", 1}}));
}

TEST(StrategyIntents, R14_RestoredZeroOpinionsAreAbsentFromTheNextIntentSnapshot)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 14, 13, 12});
    addCloses(raw, "B", {10, 14, 13, 12});
    auto engine = singleEngine();
    engine.restore(checkpoint(thirdClose, {{7, "Pure_RSI", {{"A", 0}, {"B", 1}}}}));
    const auto next = engine.onBarClose(raw, buildMarketData(raw), 20240104);
    EXPECT_EQ(intent(next, 7).signals, (Opinions{{"B", 1}}));
}

TEST(StrategyIntents, R15_OnlyReleasedCompletedSlicesCanAdvanceTheSignalEngine)
{
    OHLCVData full;
    addCloses(full, "A", {10, 14, 13, 12});
    RollingMarketState released;
    auto engine = singleEngine();
    for (Timestamp date : {20240101u, 20240102u, thirdClose}) {
        MarketSliceSnapshot closed;
        closed.timestamp = date;
        closed.bars.push_back({"A", full.data.at("A").at(date)});
        ASSERT_TRUE(released.append(closed));
        const auto batch = engine.onBarClose(released.rawData(), released.marketData(), date);
        if (date < thirdClose) EXPECT_TRUE(intent(batch, 7).signals.empty());
        else EXPECT_EQ(intent(batch, 7).signals, (Opinions{{"A", 1}}));
    }
    EXPECT_THROW(engine.onBarClose(released.rawData(), released.marketData(), 20240104),
                 std::out_of_range);
    EXPECT_EQ(engine.lastTimestamp(), thirdClose);
    MarketSliceSnapshot next;
    next.timestamp = 20240104;
    next.bars.push_back({"A", full.data.at("A").at(20240104)});
    ASSERT_TRUE(released.append(next));
    const auto held = engine.onBarClose(released.rawData(), released.marketData(), 20240104);
    EXPECT_EQ(intent(held, 7).signals, (Opinions{{"A", 1}}));
}

TEST(StrategyIntents, R15_FutureRawExtremeCannotChangeAnEarlierCompletedIntent)
{
    OHLCVData prefix;
    addCloses(prefix, "A", {10, 14, 13});
    auto full = prefix;
    full.data["A"][20240104] = {1, 1.1, 0.9, 1, 1};
    auto prefixEngine = singleEngine();
    auto fullEngine = singleEngine();
    const auto bounded = prefixEngine.onBarClose(prefix, buildMarketData(prefix), thirdClose);
    const auto withFuture = fullEngine.onBarClose(full, buildMarketData(full), thirdClose);
    EXPECT_EQ(intent(bounded, 7).signals, (Opinions{{"A", 1}}));
    EXPECT_EQ(intent(withFuture, 7).signals, intent(bounded, 7).signals);
}

TEST(StrategyIntents, R3_MissingRawTargetObservationCannotCreateOrLiquidateAnOpinion)
{
    OHLCVData complete;
    addCloses(complete, "A", {10, 14, 13});
    const auto raw = completedHistory(complete, 20240102);
    const auto market = buildMarketData(complete);
    auto fresh = singleEngine();
    const auto noEntry = fresh.onBarClose(raw, market, thirdClose);
    EXPECT_TRUE(intent(noEntry, 7).signals.empty());
    auto restored = singleEngine();
    restored.restore(checkpoint(20240102, {{7, "Pure_RSI", {{"A", 1}}}}));
    const auto retained = restored.onBarClose(raw, market, thirdClose);
    EXPECT_EQ(intent(retained, 7).signals, (Opinions{{"A", 1}}));
}

TEST(StrategyIntents, R12_ActiveSignalCapacityIsIndependentForEachStrategyId)
{
    OHLCVData raw;
    addCloses(raw, "A", {10, 14, 13});
    addCloses(raw, "B", {10, 14, 13});
    StrategySignalPortfolio portfolio;
    portfolio.emplace_back(7, makeRsi({.max_active_signals = 1}));
    portfolio.emplace_back(9, makeRsi({.max_active_signals = 2}));
    StrategySignalEngine engine(std::move(portfolio));
    const auto batch = engine.onBarClose(raw, buildMarketData(raw), thirdClose);
    EXPECT_EQ(intent(batch, 7).signals, (Opinions{{"A", 1}}));
    EXPECT_EQ(intent(batch, 9).signals, (Opinions{{"A", 1}, {"B", 1}}));
}

} // namespace
