#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "planning_test_support.h"

namespace {
using namespace planning_test;

TEST(PlanningState, R15_ReconstructionIgnoresStrategyAndAssetInsertionOrder)
{
    Inputs a;
    a.strategy_ids.push_back(kOtherStrategy);
    a.positions[kStrategy].set("BTC", 2.0);
    a.positions[kStrategy].set("ETH", -3.0);
    a.positions[kOtherStrategy].set("BTC", -1.0);
    a.orders.restore({pending()}, {101});
    const auto first = makeExecutionPlanningStateSnapshot(a.strategy_ids, a.positions, a.orders, 40);
    Inputs b;
    b.strategy_ids = {kOtherStrategy, kStrategy};
    b.positions[kOtherStrategy].set("BTC", -1.0);
    b.positions[kStrategy].set("ETH", -3.0);
    b.positions[kStrategy].set("BTC", 2.0);
    b.orders.restore({pending()}, {101});
    const auto rebuilt = makeExecutionPlanningStateSnapshot(b.strategy_ids, b.positions, b.orders, 40);
    EXPECT_NE(first.state_revision, 0U);
    EXPECT_EQ(first.state_revision, rebuilt.state_revision);
    EXPECT_EQ(first.state_revision, executionPlanningStateRevision(rebuilt));
}

TEST(PlanningState, R15_EconomicStateChangesInvalidatePriorRevision)
{
    Inputs input;
    input.strategy_ids.push_back(kOtherStrategy);
    input.positions[kStrategy].set("BTC", 2.0);
    input.orders.restore({pending()}, {101});
    const auto baseline = makeExecutionPlanningStateSnapshot(input.strategy_ids,
        input.positions, input.orders, 40);
    ASSERT_EQ(baseline.orders.size(), 1U);
    for (int change = 0; change < 12; ++change) {
        SCOPED_TRACE(change);
        auto changed = baseline;
        auto& order = changed.orders.front();
        if (change == 0) changed.strategy_positions[kStrategy]["BTC"] = 3.0;
        if (change == 1) order.filled_quantity = 3.0;
        if (change == 2) order.cancel_requested = true;
        if (change == 3) order.status = ExecutionOrderStatus::Canceled;
        if (change == 4) order.request.side = OrderSide::Sell;
        if (change == 5) order.request.quantity = 6.0;
        if (change == 6) order.request.coin = "ETH";
        if (change == 7) order.request.strategy_id = kOtherStrategy;
        if (change == 8) order.request.order_id = 11;
        if (change == 9) changed.next_order_id = 41;
        if (change == 10) changed.strategy_ids.push_back(11);
        if (change == 11) changed.orders.clear();
        EXPECT_NE(executionPlanningStateRevision(changed), baseline.state_revision);
    }
}

TEST(PlanningState, R15_OperationalLabelsAndTimestampsDoNotChangeEconomicRevision)
{
    Inputs input;
    input.orders.restore({pending()}, {});
    const auto original = makeExecutionPlanningStateSnapshot(input.strategy_ids,
        input.positions, input.orders, 40);
    auto relabeled = original;
    ASSERT_EQ(relabeled.orders.size(), 1U);
    auto& order = relabeled.orders.front();
    order.exchange_order_id = "venue-order-after-reconnect";
    order.last_message = "redelivered acknowledgement";
    order.updated_at = 20260107;
    EXPECT_EQ(executionPlanningStateRevision(relabeled), original.state_revision);
}

TEST(PlanningState, R8_SnapshotRemainsImmutableWhenAuthoritativeSourcesAdvance)
{
    Inputs input;
    input.positions[kStrategy].set("BTC", 2.0);
    input.orders.restore({pending()}, {101});
    const auto frozen = makeExecutionPlanningStateSnapshot(input.strategy_ids,
        input.positions, input.orders, 40);
    input.positions[kStrategy].set("BTC", 3.0);
    input.orders.markCancelRequested(10, kOpen);
    EXPECT_DOUBLE_EQ(frozen.strategy_positions.at(kStrategy).at("BTC"), 2.0);
    ASSERT_EQ(frozen.orders.size(), 1U);
    EXPECT_FALSE(frozen.orders.front().cancel_requested);
    EXPECT_EQ(frozen.state_revision, executionPlanningStateRevision(frozen));
    const auto advanced = makeExecutionPlanningStateSnapshot(input.strategy_ids,
        input.positions, input.orders, 40);
    EXPECT_NE(frozen.state_revision, advanced.state_revision);
}

TEST(PlanningState, R5_RestoredSnapshotRetainsPendingCoverageAndPlanIdentity)
{
    Inputs input;
    input.positions[kStrategy].set("BTC", 2.0);
    input.target("BTC", 500.0, 0.5);
    input.orders.restore({pending()}, {101});
    const auto saved = makeExecutionPlanningStateSnapshot(input.strategy_ids,
        input.positions, input.orders, 40);
    Inputs restored;
    restored.strategy_ids = saved.strategy_ids;
    restored.positions.clear();
    for (const auto& [strategy, positions] : saved.strategy_positions)
        for (const auto& [coin, quantity] : positions)
            restored.positions[strategy].set(coin, quantity);
    restored.orders.restore(saved.orders, {101});
    restored.decisions = input.decisions;
    restored.next_id = saved.next_order_id;
    const auto recovered = makeExecutionPlanningStateSnapshot(restored.strategy_ids,
        restored.positions, restored.orders, restored.next_id);
    EXPECT_EQ(saved.state_revision, recovered.state_revision);
    EXPECT_TRUE(restored.notional().submit_orders.empty());
    EXPECT_TRUE(restored.notional().cancel_order_ids.empty());
    EXPECT_TRUE(restored.quantity().execution_plan.empty());

    // After the remainder is confirmed canceled, replaying this saved boundary
    // reproduces the same replacement order identity without executing anything.
    ASSERT_TRUE(input.orders.onOrderUpdate({10, kOpen, ExecutionOrderStatus::Canceled}));
    ASSERT_TRUE(restored.orders.onOrderUpdate({10, kOpen, ExecutionOrderStatus::Canceled}));
    input.revision = makeExecutionPlanningStateSnapshot(input.strategy_ids,
        input.positions, input.orders, 40).state_revision;
    restored.revision = makeExecutionPlanningStateSnapshot(restored.strategy_ids,
        restored.positions, restored.orders, 40).state_revision;
    const auto a = input.notional();
    const auto b = restored.notional();
    ASSERT_EQ(a.submit_orders.size(), 1U);
    ASSERT_EQ(b.submit_orders.size(), 1U);
    EXPECT_EQ(a.submit_orders.front().economic_order_id, b.submit_orders.front().economic_order_id);
    EXPECT_DOUBLE_EQ(a.submit_orders.front().notional_usd, 300.0);
    EXPECT_DOUBLE_EQ(b.submit_orders.front().notional_usd, 300.0);
}

TEST(PlanningState, R15_LivePositionStopReplacementDoesNotInvalidatePlanningSnapshot)
{
    Inputs input;
    input.positions[kStrategy].set("BTC", 2.0);
    const auto withoutProtection = makeExecutionPlanningStateSnapshot(input.strategy_ids,
        input.positions, input.orders, 40);
    auto stop = pending(OrderSide::Sell, 2.0, 0.0);
    stop.request.position_stop_price = 90.0;
    input.orders.restore({stop}, {});
    const auto protectedState = makeExecutionPlanningStateSnapshot(input.strategy_ids,
        input.positions, input.orders, 40);
    EXPECT_EQ(withoutProtection.state_revision, protectedState.state_revision);
    EXPECT_TRUE(protectedState.orders.empty());
    stop.request.order_id = 11;
    stop.request.position_stop_price = 95.0;
    input.orders.restore({stop}, {});
    const auto replaced = makeExecutionPlanningStateSnapshot(input.strategy_ids,
        input.positions, input.orders, 40);
    EXPECT_EQ(withoutProtection.state_revision, replaced.state_revision);
}

TEST(PlanningState, R15_ConditionalPriceAndActivationChangesInvalidateRevision)
{
    Inputs input;
    auto stop = pending(OrderSide::Buy, 2.5, 0.0);
    stop.request.entry_stop_price = 100.0;
    input.orders.restore({stop}, {});
    const auto baseline = makeExecutionPlanningStateSnapshot(input.strategy_ids,
        input.positions, input.orders, 40);
    for (int change = 0; change < 3; ++change) {
        auto changed = baseline;
        ASSERT_EQ(changed.orders.size(), 1U);
        if (change == 0) changed.orders.front().request.entry_stop_price = 101.0;
        if (change == 1) changed.orders.front().request.created_at = 20260104;
        if (change == 2) changed.orders.front().request.active_from = 20260107;
        EXPECT_NE(executionPlanningStateRevision(changed), baseline.state_revision);
    }
}

TEST(PlanningState, R15_OrderVectorPermutationDoesNotChangeRevisionOrPendingCoverage)
{
    Inputs input;
    input.positions[kStrategy].set("BTC", 2.0);
    input.positions[kStrategy].set("ETH", 1.0);
    input.orders.restore({pending(), pending(OrderSide::Sell, 4.0, 1.0, 11, kStrategy, "ETH")}, {});
    const auto first = makeExecutionPlanningStateSnapshot(input.strategy_ids,
        input.positions, input.orders, 40);
    auto permuted = first;
    ASSERT_EQ(permuted.orders.size(), 2U);
    std::reverse(permuted.orders.begin(), permuted.orders.end());
    EXPECT_EQ(executionPlanningStateRevision(permuted), first.state_revision);
    OrderManager restored;
    restored.restore(permuted.orders, {});
    EXPECT_DOUBLE_EQ(restored.pendingSignedQuantity(kStrategy, "BTC"), 3.0);
    EXPECT_DOUBLE_EQ(restored.pendingSignedQuantity(kStrategy, "ETH"), -3.0);
}

} // namespace
