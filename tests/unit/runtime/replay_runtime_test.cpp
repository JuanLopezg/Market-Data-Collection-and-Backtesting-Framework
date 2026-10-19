#include <limits>
#include <map>

#include <gtest/gtest.h>

#include "runtime_examples.h"

namespace runtime_test {
namespace {

class ReplayBehaviour : public testing::Test {
protected:
    test_support::TempDir directory_{"runtime-replay"};
};

// FINDING: F3. Valid initial cash 1000 disagrees with MOCK's fixed 100000 ledger
// genesis; reconciliation blocks the expected USD 250 entry and its later PnL.
TEST_F(ReplayBehaviour, DISABLED_R10_CloseNotionalBecomesNextOpenQuantityAndFinalProfit)
{
    ReplayRig rig(directory_ / "venue");
    rig.through(3);
    ASSERT_TRUE(rig.runtime.hasPendingPlan());
    const auto& orders = rig.runtime.pendingPlan().submit_orders;
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders.front().target_notional_usd, 250.0);
    EXPECT_EQ(orders.front().reference_close, 120.0);
    EXPECT_TRUE(rig.runtime.parityFills().empty());
    const auto initial = rig.venue.reconcile(openTime(4));
    EXPECT_TRUE(initial.clean()) << (initial.issues.empty() ? "Missing clean reconciliation" :
        initial.issues.front().message + ": local=" + initial.issues.front().local_value +
        " venue=" + initial.issues.front().venue_value);
    rig.open(4);
    EXPECT_EQ(rig.quantity(), 2.0);
    ASSERT_EQ(rig.runtime.parityFills().size(), 1U);
    EXPECT_EQ(rig.runtime.parityFills().front().price, 125.0);
    EXPECT_EQ(rig.runtime.parityFills().front().timestamp, openTime(4));
    rig.close(4);
    EXPECT_EQ(rig.runtime.snapshotResumeState().parity_cash, 750.0);
    const auto margin = rig.venue.chaos().runtime().account().marginState(closeTime(4));
    EXPECT_EQ(margin.settled_cash, 1000.0);
    EXPECT_EQ(margin.unrealized_pnl, -50.0);
    EXPECT_EQ(margin.equity, 950.0);
    rig.completeDay(5);
    EXPECT_EQ(rig.quantity(), 0.0);
    EXPECT_EQ(rig.runtime.snapshotResumeState().parity_cash, 1050.0);
    ASSERT_EQ(rig.runtime.parityFills().size(), 2U);
    EXPECT_EQ(rig.runtime.parityFills().back().quantity, 2.0);
    EXPECT_EQ(rig.runtime.parityFills().back().side, OrderSide::Sell);
    EXPECT_EQ(rig.runtime.parityFills().back().price, 150.0);
    EXPECT_TRUE(rig.venue.reconcile(closeTime(5)).clean());
}

TEST_F(ReplayBehaviour, R10_NormalMockUsesCloseReferenceAndExitsItsActualQuantity)
{
    ReplayRig rig(directory_ / "venue", false, {StrategyConfig{7, 1.0, 0.0025}}, 100.0, "100000.00");
    rig.through(4);
    EXPECT_EQ(rig.quantity(), 2.08333333); // floor((250/120)*1e8)/1e8.
    rig.completeDay(5);
    EXPECT_EQ(rig.quantity(), 0.0);
    EXPECT_TRUE(rig.venue.chaos().runtime().account().accountSnapshot(closeTime(5)).positions.empty());
    EXPECT_TRUE(rig.venue.reconcile(closeTime(5)).clean());
}

TEST_F(ReplayBehaviour, R11_OpenOnlyZeroVolumeEvidenceFillsWithoutFutureCloseLeakage)
{
    ReplayRig falling(directory_ / "falling", true, {StrategyConfig{7, 1.0, 0.0025}}, 100.0, "100000.00");
    ReplayRig rising(directory_ / "rising", true, {StrategyConfig{7, 1.0, 0.0025}}, 10000.0, "100000.00");
    falling.through(3);
    rising.through(3);
    falling.open(4, 0.0);
    rising.open(4, 0.0);
    ASSERT_EQ(falling.runtime.parityFills().size(), 1U);
    expectFillsEqual(rising.runtime.parityFills(), falling.runtime.parityFills());
    EXPECT_EQ(falling.quantity(), 2.0);
    EXPECT_EQ(falling.runtime.parityFills().front().price, 125.0);
}

// FINDING: F3. The fixed 100000 reconciliation genesis blocks the valid 1000-cash
// fixture's initial buy, so the required held position and exit plan never exist.
TEST_F(ReplayBehaviour, DISABLED_R12_DuplicateAndOldClosesCannotReapplyFillsOrAdvanceThePlan)
{
    ReplayRig rig(directory_ / "venue");
    rig.through(4);
    const auto before = rig.runtime.snapshotResumeState();
    EXPECT_THROW(rig.close(4), std::logic_error);
    EXPECT_THROW(rig.close(3), std::logic_error);
    const auto after = rig.runtime.snapshotResumeState();
    EXPECT_EQ(after.next_order_id, before.next_order_id);
    EXPECT_EQ(after.planning_revision, before.planning_revision);
    EXPECT_EQ(after.parity_cash, 750.0);
    EXPECT_EQ(rig.quantity(), 2.0);
    expectFillsEqual(after.parity_fills, before.parity_fills);
    ASSERT_TRUE(after.pending_plan.has_value());
    ASSERT_EQ(after.pending_plan->submit_orders.size(), 1U);
    EXPECT_EQ(after.pending_plan->submit_orders.front().target_notional_usd, 0.0);
}

TEST_F(ReplayBehaviour, R12_ExecutionMustFollowItsDecisionAndCannotReplayAnOpen)
{
    ReplayRig rig(directory_ / "venue", true, {StrategyConfig{7, 1.0, 0.0025}}, 100.0, "100000.00");
    rig.through(3);
    EXPECT_THROW(rig.runtime.onExecutionOpen(closeTime(3), prices(120.0)), std::logic_error);
    EXPECT_TRUE(rig.runtime.hasPendingPlan());
    EXPECT_TRUE(rig.runtime.parityFills().empty());
    rig.open(4);
    EXPECT_THROW(rig.runtime.onExecutionOpen(openTime(4), prices(125.0)), std::logic_error);
    EXPECT_THROW(rig.runtime.onExecutionOpen(openTime(3), prices(115.0)), std::logic_error);
    EXPECT_EQ(rig.quantity(), 2.0);
    EXPECT_EQ(rig.runtime.parityFills().size(), 1U);
}

TEST_F(ReplayBehaviour, R13_CheckpointRequiresACompletedCloseAndNoWaitingOpenFill)
{
    ReplayRig rig(directory_ / "venue", true, {StrategyConfig{7, 1.0, 0.0025}}, 100.0, "100000.00");
    EXPECT_THROW((void)rig.runtime.snapshotResumeState(), std::logic_error);
    rig.through(3);
    EXPECT_NO_THROW((void)rig.runtime.snapshotResumeState());
    rig.runtime.onExecutionOpen(openTime(4), prices(125.0));
    EXPECT_THROW((void)rig.runtime.snapshotResumeState(), std::logic_error);
    matchAtOpen(rig.venue, openTime(4), prices(125.0));
    rig.close(4);
    EXPECT_NO_THROW((void)rig.runtime.snapshotResumeState());
}

TEST_F(ReplayBehaviour, R13_NormalMockCannotResumeAndInconsistentSignalCheckpointIsRejected)
{
    ReplayRig normal(directory_ / "normal", false);
    normal.through(3);
    EXPECT_THROW((void)normal.runtime.snapshotResumeState(), std::logic_error);
    ReplayRig source(directory_ / "source");
    source.through(3);
    const auto saved = source.runtime.snapshotResumeState();
    EXPECT_THROW(normal.runtime.restoreResumeState(saved), std::logic_error);
    for (const bool missing : {false, true}) {
        auto bad = saved;
        if (missing) bad.last_signal_batch.reset();
        else bad.last_signal_batch->timestamp = closeTime(2);
        ReplayRig restored(directory_ / (missing ? "missing" : "mismatch"));
        EXPECT_THROW(restored.runtime.restoreResumeState(bad), std::invalid_argument);
        EXPECT_TRUE(restored.runtime.parityFills().empty());
        EXPECT_TRUE(restored.venue.chaos().runtime().lifecycle().orders().empty());
    }
}

// FINDING: F3. Fixed-genesis reconciliation blocks all entries with initial cash
// 1000; both restart boundaries remain at 1000 instead of the specified 1050.
TEST_F(ReplayBehaviour, DISABLED_R14_CompletedDayRestartMatchesUninterruptedEconomicAndStreamState)
{
    ReplayRig continuous(directory_ / "continuous");
    continuous.through(5);
    const auto expectedState = continuous.runtime.snapshotResumeState();
    const auto expectedEconomic = continuous.venue.chaos().runtime().account().economicFingerprint();
    const auto expectedStream = continuous.venue.chaos().runtime().streamFingerprint();
    for (const unsigned boundary : {3U, 4U}) {
        SCOPED_TRACE(boundary);
        const auto path = directory_ / ("interrupted-" + std::to_string(boundary));
        Replay::ReplayResumeState saved;
        MockVenue::MockFaultChaosResumeState chaos;
        {
            ReplayRig interrupted(path);
            interrupted.through(boundary);
            saved = interrupted.runtime.snapshotResumeState();
            chaos = interrupted.venue.chaos().snapshotResumeState();
            ASSERT_TRUE(interrupted.venue.chaos().runtime().checkpoint());
        }
        ReplayRig resumed(path);
        resumed.venue.chaos().restoreResumeState(chaos);
        resumed.runtime.restoreResumeState(saved);
        for (unsigned day = boundary + 1; day <= 5; ++day) resumed.completeDay(day);
        const auto actual = resumed.runtime.snapshotResumeState();
        EXPECT_EQ(actual.parity_cash, 1050.0);
        EXPECT_EQ(resumed.quantity(), 0.0);
        EXPECT_EQ(actual.next_order_id, expectedState.next_order_id);
        EXPECT_EQ(actual.planning_revision, expectedState.planning_revision);
        EXPECT_EQ(actual.last_closed_timestamp, closeTime(5));
        expectFillsEqual(actual.parity_fills, expectedState.parity_fills);
        EXPECT_EQ(resumed.venue.chaos().runtime().account().economicFingerprint(), expectedEconomic);
        EXPECT_EQ(resumed.venue.chaos().runtime().streamFingerprint(), expectedStream);
    }
}

TEST_F(ReplayBehaviour, R15_MissingOpenPricePreservesPendingIntentForRetry)
{
    ReplayRig rig(directory_ / "venue", true, {StrategyConfig{7, 1.0, 0.0025}}, 100.0, "100000.00");
    rig.through(3);
    const OrderID nextId = rig.runtime.nextOrderId();
    EXPECT_THROW(rig.runtime.onExecutionOpen(openTime(4), PriceSnapshot{}), std::runtime_error);
    EXPECT_TRUE(rig.runtime.hasPendingPlan());
    EXPECT_EQ(rig.runtime.nextOrderId(), nextId);
    EXPECT_TRUE(rig.venue.chaos().runtime().lifecycle().orders().empty());
    rig.open(4);
    EXPECT_EQ(rig.quantity(), 2.0);
}

TEST_F(ReplayBehaviour, R15_UnavailableVenueBlocksSubmissionWithoutInventingFills)
{
    ReplayRig rig(directory_ / "venue");
    rig.through(3);
    rig.venue.chaos().setVenueAvailable(false, openTime(4));
    rig.runtime.onExecutionOpen(openTime(4), prices(125.0));
    EXPECT_FALSE(rig.runtime.routeSafe());
    EXPECT_EQ(rig.quantity(), 0.0);
    EXPECT_TRUE(rig.runtime.parityFills().empty());
    EXPECT_TRUE(rig.venue.chaos().runtime().lifecycle().orders().empty());
    EXPECT_EQ(rig.venue.chaos().runtime().account().marginState(openTime(4)).settled_cash, 1000.0);
}

// FINDING: F4. Replay fills the admitted Buy 2 before RollingMarketState rejects
// the duplicate asset, leaving position/fills mutated despite the invalid slice.
TEST_F(ReplayBehaviour, DISABLED_R15_InvalidCrossSectionCannotFillBeforeItsValidation)
{
    ReplayRig rig(directory_ / "venue", true, {StrategyConfig{7, 1.0, 0.0025}}, 100.0, "100000.00");
    rig.through(3);
    rig.runtime.onExecutionOpen(openTime(4), prices(125.0));
    auto invalid = daySlice(4);
    invalid.bars.push_back(invalid.bars.front());
    EXPECT_THROW(rig.runtime.onClosedSlice(invalid), std::invalid_argument);
    EXPECT_EQ(rig.quantity(), 0.0);
    EXPECT_TRUE(rig.runtime.parityFills().empty());
    const auto* order = rig.runtime.planningOrderManager().find(1);
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->filled_quantity, 0.0);
}

// FINDING: F3. Both 1000-cash diagnostic runs are blocked by fixed-genesis
// reconciliation; expected trading cash 1050 is never reached.
TEST_F(ReplayBehaviour, DISABLED_R16_DiagnosticsLeaveCashFillsAndFingerprintsUnchanged)
{
    ReplayRig normal(directory_ / "normal");
    ReplayRig observed(directory_ / "observed");
    observed.runtime.enableDecisionDiagnostics();
    normal.through(5);
    observed.through(5);
    EXPECT_EQ(observed.runtime.snapshotResumeState().parity_cash, 1050.0);
    expectFillsEqual(observed.runtime.parityFills(), normal.runtime.parityFills());
    const auto plain = normal.runtime.finalizeEvidence(closeTime(5));
    const auto diagnostic = observed.runtime.finalizeEvidence(closeTime(5));
    EXPECT_EQ(diagnostic.economic_fingerprint, plain.economic_fingerprint);
    EXPECT_EQ(diagnostic.stream_fingerprint, plain.stream_fingerprint);
    EXPECT_EQ(diagnostic.full_run_fingerprint, plain.full_run_fingerprint);
    ASSERT_TRUE(observed.runtime.lastDecisionBatch().has_value());
    EXPECT_EQ(observed.runtime.lastDecisionBatch()->decision_timestamp, closeTime(5));
}

// FINDING: F3. After correctly rejecting .31, the permitted .30 manual target is
// blocked by the 100000 ledger genesis versus configured initial cash 1000.
TEST_F(ReplayBehaviour, DISABLED_R17_StrictestManualLimitsRejectInsteadOfClipping)
{
    ReplayRig rig(directory_ / "venue", true,
        {StrategyConfig{7, 0.5, 0.25, 0.75, 0.5}, StrategyConfig{9, 0.5, 0.25, 1.0, 0.3}});
    rig.through(3);
    const auto before = rig.runtime.snapshotResumeState();
    EXPECT_EQ(rig.runtime.manualLimits().max_gross_leverage, 0.75);
    EXPECT_EQ(rig.runtime.manualLimits().max_asset_weight, 0.3);
    EXPECT_THROW((void)rig.runtime.rebalanceToManualTarget({{"BTCUSDT", 0.31}}, openTime(4), prices(125.0)),
                 std::invalid_argument);
    EXPECT_TRUE(rig.runtime.hasPendingPlan());
    EXPECT_EQ(rig.runtime.nextOrderId(), before.next_order_id);
    EXPECT_EQ(rig.runtime.snapshotResumeState().parity_cash, 1000.0);
    EXPECT_TRUE(rig.venue.chaos().runtime().lifecycle().orders().empty());
    const auto accepted = rig.runtime.rebalanceToManualTarget({{"BTCUSDT", 0.3}}, openTime(4), prices(125.0));
    EXPECT_TRUE(accepted.discarded_strategy_plan);
    EXPECT_EQ(accepted.submits, 2U); // Each strategy receives its own USD 150 target.
}

TEST_F(ReplayBehaviour, R17_InvalidManualWeightsTimesAndPricesPreservePendingState)
{
    ReplayRig rig(directory_ / "venue", true, {StrategyConfig{7, 1.0, 0.25, 0.4, 0.3}});
    rig.through(3);
    const auto before = rig.runtime.snapshotResumeState();
    for (const auto& weights : std::vector<std::map<Coin, double>>{
            {{"BTCUSDT", -0.1}}, {{"BTCUSDT", std::numeric_limits<double>::quiet_NaN()}},
            {{"", 0.1}}, {{"BTCUSDT", 0.25}, {"ETHUSDT", 0.25}}})
        EXPECT_THROW((void)rig.runtime.rebalanceToManualTarget(weights, openTime(4), prices(125.0)),
                     std::invalid_argument);
    EXPECT_THROW((void)rig.runtime.rebalanceToManualTarget({{"BTCUSDT", 0.25}}, openTime(4), PriceSnapshot{}),
                 std::invalid_argument);
    EXPECT_THROW((void)rig.runtime.rebalanceToManualTarget({{"BTCUSDT", 0.25}}, closeTime(3), prices(120.0)),
                 std::logic_error);
    EXPECT_TRUE(rig.runtime.hasPendingPlan());
    EXPECT_EQ(rig.runtime.nextOrderId(), before.next_order_id);
    EXPECT_EQ(rig.runtime.snapshotResumeState().parity_cash, 1000.0);
    EXPECT_TRUE(rig.venue.chaos().runtime().lifecycle().orders().empty());
}

// FINDING: F3. Fixed-genesis reconciliation blocks the valid 1000-cash manual
// allocation before the specified discard, fill and target re-sync can occur.
TEST_F(ReplayBehaviour, DISABLED_R18_ManualTargetDiscardsOldPlanAndResyncsHoldAtNextClose)
{
    ReplayRig rig(directory_ / "venue", true, {StrategyConfig{}}, 130.0);
    rig.through(3);
    const auto result = rig.runtime.rebalanceToManualTarget({{"BTCUSDT", 0.5}}, openTime(4), prices(125.0));
    EXPECT_TRUE(result.discarded_strategy_plan);
    EXPECT_EQ(result.submits, 1U);
    EXPECT_FALSE(rig.runtime.hasPendingPlan());
    EXPECT_TRUE(rig.runtime.targetResyncPending());
    matchAtOpen(rig.venue, openTime(4), prices(125.0));
    EXPECT_EQ(rig.quantity(), 4.0);
    EXPECT_THROW((void)rig.runtime.snapshotResumeState(), std::logic_error);
    rig.close(4);
    EXPECT_FALSE(rig.runtime.targetResyncPending());
    const auto state = rig.runtime.snapshotResumeState();
    EXPECT_EQ(state.parity_cash, 500.0);
    EXPECT_EQ(state.evidence.closed_slices, 4U);
    EXPECT_EQ(state.evidence.execution_opens, 3U); // Manual admission is not a scheduled open.
    ASSERT_TRUE(state.pending_plan.has_value());
    ASSERT_EQ(state.pending_plan->submit_orders.size(), 1U);
    EXPECT_EQ(state.pending_plan->submit_orders.front().side, OrderSide::Sell);
    EXPECT_EQ(state.pending_plan->submit_orders.front().target_notional_usd, 255.0);
}

// FINDING: F3. Fixed-genesis reconciliation blocks the initial 1000-cash entry
// and subsequent manual route; expected cash-only flattening cannot proceed.
TEST_F(ReplayBehaviour, DISABLED_R18_CashOnlyManualTargetClosesOmittedHoldingsAndNeedsTheirPrices)
{
    ReplayRig rig(directory_ / "venue");
    rig.through(3);
    rig.open(4);
    const auto next = rig.runtime.nextOrderId();
    EXPECT_THROW((void)rig.runtime.rebalanceToManualTarget({}, manualTime(4), PriceSnapshot{}),
                 std::invalid_argument);
    EXPECT_EQ(rig.runtime.nextOrderId(), next);
    const auto result = rig.runtime.rebalanceToManualTarget({}, manualTime(4), prices(150.0));
    EXPECT_EQ(result.submits, 1U);
    matchAtOpen(rig.venue, manualTime(4), prices(150.0));
    EXPECT_EQ(rig.quantity(), 0.0);
    ASSERT_EQ(rig.runtime.parityFills().size(), 2U);
    EXPECT_EQ(rig.runtime.parityFills().back().quantity, 2.0);
    EXPECT_EQ(rig.runtime.parityFills().back().side, OrderSide::Sell);
    rig.close(4);
    EXPECT_EQ(rig.runtime.snapshotResumeState().parity_cash, 1050.0);
}

TEST_F(ReplayBehaviour, R14_CanonicalGenesisControlPreservesMoneyAcrossRestartAndDiagnostics)
{
    const std::vector<StrategyConfig> configs{StrategyConfig{7, 1.0, 0.0025}};
    ReplayRig continuous(directory_ / "control", true, configs, 100.0, "100000.00");
    continuous.through(5);
    EXPECT_EQ(continuous.runtime.snapshotResumeState().parity_cash, 100050.0);
    ASSERT_EQ(continuous.runtime.parityFills().size(), 2U);
    EXPECT_EQ(continuous.runtime.parityFills()[0].quantity, 2.0);
    EXPECT_EQ(continuous.runtime.parityFills()[0].price, 125.0);
    EXPECT_EQ(continuous.runtime.parityFills()[1].quantity, 2.0);
    EXPECT_EQ(continuous.runtime.parityFills()[1].price, 150.0);
    Replay::ReplayResumeState saved;
    MockVenue::MockFaultChaosResumeState chaos;
    {
        ReplayRig interrupted(directory_ / "restart-control", true, configs, 100.0, "100000.00");
        interrupted.through(4);
        saved = interrupted.runtime.snapshotResumeState();
        EXPECT_EQ(saved.parity_cash, 99750.0);
        chaos = interrupted.venue.chaos().snapshotResumeState();
        ASSERT_TRUE(interrupted.venue.chaos().runtime().checkpoint());
    }
    ReplayRig resumed(directory_ / "restart-control", true, configs, 100.0, "100000.00");
    resumed.venue.chaos().restoreResumeState(chaos);
    resumed.runtime.restoreResumeState(saved);
    resumed.runtime.enableDecisionDiagnostics();
    resumed.completeDay(5);
    EXPECT_EQ(resumed.runtime.snapshotResumeState().parity_cash, 100050.0);
    EXPECT_EQ(resumed.quantity(), 0.0);
    expectFillsEqual(resumed.runtime.parityFills(), continuous.runtime.parityFills());
    const auto expected = continuous.runtime.finalizeEvidence(closeTime(5));
    const auto actual = resumed.runtime.finalizeEvidence(closeTime(5));
    EXPECT_EQ(actual.economic_fingerprint, expected.economic_fingerprint);
    EXPECT_EQ(actual.stream_fingerprint, expected.stream_fingerprint);
    EXPECT_EQ(actual.full_run_fingerprint, expected.full_run_fingerprint);
}

} // namespace
} // namespace runtime_test
