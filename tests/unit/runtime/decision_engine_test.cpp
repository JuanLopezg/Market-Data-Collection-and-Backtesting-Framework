#include <gtest/gtest.h>

#include "runtime_examples.h"

namespace runtime_test {
namespace {

class DecisionBehaviour : public testing::Test {
protected:
    TradingRig rig_;
    DecisionEngine decision_{rig_.strategies, rig_.indicators};
    StrategyPositionSnapshot positions_{{7, VirtualPositionState{}}};

    void close(unsigned day, double equity = 1000.0)
    {
        decision_.onBarClose(rig_.history.marketData(), closeTime(day), equity, positions_);
    }
};

TEST_F(DecisionBehaviour, R1_WarmupHasNoEntryButThirdCompletedCloseApproves250Dollars)
{
    close(1);
    EXPECT_FALSE(decision_.hasPendingDecisions());
    close(2);
    EXPECT_FALSE(decision_.hasPendingDecisions());
    close(3);
    const auto& batch = decision_.pendingDecisions();
    ASSERT_EQ(batch.strategies.size(), 1U);
    EXPECT_EQ(batch.decision_timestamp, closeTime(3));
    const auto& intent = batch.strategies.front();
    EXPECT_EQ(intent.strategy_id, 7U);
    EXPECT_EQ(intent.reference_capital, 1000.0);
    EXPECT_EQ(intent.decisions.at("BTCUSDT").action, RebalanceAction::TargetWeight);
    EXPECT_EQ(intent.decisions.at("BTCUSDT").target_weight, 0.25);
    EXPECT_EQ(intent.reference_capital * intent.decisions.at("BTCUSDT").target_weight, 250.0);
    EXPECT_TRUE(positions_.at(7).values().empty());
    expectAccount(rig_, 1000.0, 0.0);
}

TEST_F(DecisionBehaviour, R1_ChangingFuturePricesCannotChangeTheEarlierDecision)
{
    TradingRig future({StrategyConfig{}}, 0.0, 10000.0);
    DecisionEngine other(future.strategies, future.indicators);
    close(3);
    other.onBarClose(future.history.marketData(), closeTime(3), 1000.0, positions_);
    const auto& before = decision_.pendingDecisions().strategies;
    const auto& changed = other.pendingDecisions().strategies;
    ASSERT_EQ(before.size(), 1U);
    ASSERT_EQ(changed.size(), 1U);
    EXPECT_EQ(changed.front().reference_capital, 1000.0);
    EXPECT_EQ(changed.front().decisions.at("BTCUSDT").target_weight, 0.25);
    EXPECT_EQ(changed.front().decisions.at("BTCUSDT").action, before.front().decisions.at("BTCUSDT").action);
    EXPECT_EQ(future.strategies.front().signalState().get("BTCUSDT"), 1.0);
}

TEST_F(DecisionBehaviour, R2_AllocationAndAssetCapApplyBeforeQuantityResolution)
{
    TradingRig allocated({StrategyConfig{7, 0.5, 0.25, 1.0, 1.0}});
    DecisionEngine allocation(allocated.strategies, allocated.indicators);
    allocation.onBarClose(allocated.history.marketData(), closeTime(3), 1000.0, positions_);
    const auto& allocatedIntents = allocation.pendingDecisions().strategies;
    ASSERT_EQ(allocatedIntents.size(), 1U);
    EXPECT_EQ(allocatedIntents.front().reference_capital, 500.0);
    EXPECT_EQ(allocatedIntents.front().reference_capital *
              allocatedIntents.front().decisions.at("BTCUSDT").target_weight, 125.0);

    TradingRig capped({StrategyConfig{7, 1.0, 0.25, 1.0, 0.10}});
    DecisionEngine cap(capped.strategies, capped.indicators);
    cap.onBarClose(capped.history.marketData(), closeTime(3), 1000.0, positions_);
    const auto& cappedIntents = cap.pendingDecisions().strategies;
    ASSERT_EQ(cappedIntents.size(), 1U);
    EXPECT_EQ(cappedIntents.front().reference_capital, 1000.0);
    EXPECT_EQ(cappedIntents.front().decisions.at("BTCUSDT").target_weight, 0.10);
    EXPECT_EQ(cappedIntents.front().reference_capital *
              cappedIntents.front().decisions.at("BTCUSDT").target_weight, 100.0);
}

TEST_F(DecisionBehaviour, R3_HoldLeavesFilledUnitsUntouchedAndExitProducesFlat)
{
    positions_.at(7).set("BTCUSDT", 2.0);
    close(3);
    EXPECT_FALSE(decision_.hasPendingDecisions());
    EXPECT_EQ(positions_.at(7).get("BTCUSDT"), 2.0);
    close(4, 950.0);
    const auto& intents = decision_.pendingDecisions().strategies;
    ASSERT_EQ(intents.size(), 1U);
    EXPECT_EQ(intents.front().reference_capital, 950.0);
    EXPECT_EQ(intents.front().decisions.at("BTCUSDT").action, RebalanceAction::Flat);
    EXPECT_EQ(positions_.at(7).get("BTCUSDT"), 2.0); // Intent cannot apply its own exit.
}

TEST_F(DecisionBehaviour, R4_NewerActionableDecisionReplacesOnePendingStrategyIntent)
{
    TradingRig rising({StrategyConfig{}}, 0.0, 130.0);
    DecisionEngine decision(rising.strategies, rising.indicators);
    decision.onBarClose(rising.history.marketData(), closeTime(3), 1000.0, positions_);
    decision.onBarClose(rising.history.marketData(), closeTime(4), 1100.0, positions_);
    const auto& batch = decision.pendingDecisions();
    ASSERT_EQ(batch.strategies.size(), 1U);
    EXPECT_EQ(batch.decision_timestamp, closeTime(4));
    EXPECT_EQ(batch.strategies.front().decision_timestamp, closeTime(4));
    EXPECT_EQ(batch.strategies.front().reference_capital, 1100.0);
    EXPECT_EQ(batch.strategies.front().reference_capital *
              batch.strategies.front().decisions.at("BTCUSDT").target_weight, 275.0);
}

// FINDING: F1. The new flat signal produces an empty plan, but DecisionEngine
// retains the older unexecuted entry instead of removing it as required by R4.
TEST_F(DecisionBehaviour, DISABLED_R4_NewFlatSignalRemovesAnUnexecutedOlderEntry)
{
    close(3);
    ASSERT_TRUE(decision_.hasPendingDecisions());
    close(4);
    EXPECT_EQ(rig_.strategies.front().signalState().get("BTCUSDT"), 0.0);
    EXPECT_FALSE(decision_.hasPendingDecisions());
    EXPECT_TRUE(decision_.pendingDecisions().strategies.empty());
    EXPECT_TRUE(positions_.at(7).values().empty());
}

TEST_F(DecisionBehaviour, R4_ClearingPendingIntentPreservesThePersistentSignal)
{
    close(3);
    decision_.clearPendingDecisions();
    EXPECT_FALSE(decision_.hasPendingDecisions());
    EXPECT_EQ(rig_.strategies.front().signalState().get("BTCUSDT"), 1.0);
    close(4);
    EXPECT_FALSE(decision_.hasPendingDecisions());
    EXPECT_EQ(rig_.strategies.front().signalState().get("BTCUSDT"), 0.0);
}

TEST_F(DecisionBehaviour, R3_HeldAssetOutsideEntryUniverseStillGetsItsExit)
{
    positions_.at(7).set("BTCUSDT", 2.0);
    close(3);
    MarketData entryUniverse;
    BarData eth;
    eth.open = eth.high = eth.low = eth.close = 100.0;
    entryUniverse[closeTime(4)].emplace("ETHUSDT", eth);
    decision_.onBarClose(entryUniverse, rig_.history.marketData(), closeTime(4), 950.0, positions_);
    const auto& intents = decision_.pendingDecisions().strategies;
    ASSERT_EQ(intents.size(), 1U);
    EXPECT_EQ(intents.front().decisions.at("BTCUSDT").action, RebalanceAction::Flat);
    EXPECT_EQ(intents.front().reference_capital, 950.0);
    EXPECT_EQ(positions_.at(7).get("BTCUSDT"), 2.0);
}

} // namespace
} // namespace runtime_test
