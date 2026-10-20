#include <gtest/gtest.h>

#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

#include "execution_test_support.h"

namespace {
using namespace execution_test;

TEST(ExecutionEngine, R11_AllCancelAndSubmitIntentsAreDurableBeforeFirstCommand)
{
    Harness<> h;
    h.manual(order(1));
    auto eth = order(3, OrderSide::Buy, 3.0);
    eth.coin = "ETH";
    auto batch = plan({order(2, OrderSide::Sell, 2.0, kBeta), eth}, 4);
    batch.cancel_order_ids = {1};
    batch.global_target_exposure["ETH"] = 150.0;
    bool firstCommit = true;
    h.engine.applyOrderPlan(batch, [&](const std::optional<Fill>& incoming) {
        EXPECT_FALSE(incoming.has_value());
        if (firstCommit) {
            firstCommit = false;
            EXPECT_EQ(h.exchange.submitCount(), 1U);
            EXPECT_EQ(h.exchange.cancelRequestCount(1), 0U);
            ASSERT_NE(h.engine.orderManager().find(1), nullptr);
            ASSERT_NE(h.engine.orderManager().find(2), nullptr);
            ASSERT_NE(h.engine.orderManager().find(3), nullptr);
            EXPECT_TRUE(h.engine.orderManager().find(1)->cancel_requested);
            EXPECT_EQ(h.engine.orderManager().find(2)->status, ExecutionOrderStatus::Created);
            EXPECT_EQ(h.engine.orderManager().find(3)->status, ExecutionOrderStatus::Created);
            EXPECT_EQ(h.engine.nextOrderId(), 4U);
            EXPECT_EQ(h.engine.lastExecutionTimestamp(), kOpen);
        }
        h.capture(incoming);
    });
    EXPECT_FALSE(firstCommit);
    EXPECT_EQ(h.exchange.submitCount(), 3U);
    EXPECT_EQ(h.exchange.cancelRequestCount(1), 1U);
    EXPECT_EQ(h.engine.orderManager().find(2)->status, ExecutionOrderStatus::Submitted);
    EXPECT_EQ(h.engine.orderManager().find(3)->status, ExecutionOrderStatus::Submitted);
    EXPECT_DOUBLE_EQ(h.engine.lastGlobalTarget().get("ETH"), 150.0);
    EXPECT_DOUBLE_EQ(h.account.cash(), 1000.0);
    EXPECT_TRUE(h.account.positions().values().empty());
}

TEST(ExecutionEngine, R12_InvalidSubmitIdentityOrDateRejectsEntirePlan)
{
    for (int invalid = 0; invalid < 7; ++invalid) {
        SCOPED_TRACE(invalid);
        Harness<> h;
        h.manual(order(1));
        auto batch = plan({order(2)}, 3);
        if (invalid == 0) batch.execution_timestamp = 0;
        if (invalid == 1) batch.next_order_id = 0;
        if (invalid == 2) batch.next_order_id = 1;
        if (invalid == 3) batch.submit_orders.push_back(order(2));
        if (invalid == 4) batch.submit_orders.push_back(order(3, OrderSide::Buy, 2.0, 99));
        if (invalid == 4) batch.next_order_id = 4;
        if (invalid == 5) batch.submit_orders.front().order_id = 1;
        if (invalid == 6) batch.submit_orders.front().order_id = 3;
        const auto committed = h.saved.size();
        EXPECT_THROW(h.engine.applyOrderPlan(batch, h.persist()), std::invalid_argument);
        EXPECT_EQ(h.saved.size(), committed);
        EXPECT_EQ(h.exchange.submitCount(), 1U);
        EXPECT_EQ(h.engine.orderManager().orders().size(), 1U);
        EXPECT_EQ(h.engine.nextOrderId(), 2U);
        EXPECT_EQ(h.engine.lastExecutionTimestamp(), 0U);
        EXPECT_FALSE(h.engine.orderManager().find(1)->cancel_requested);
        EXPECT_DOUBLE_EQ(h.account.cash(), 1000.0);
    }
}

TEST(ExecutionEngine, R12_InvalidCancelIntentRejectsPlanBeforeStaging)
{
    for (int invalid = 0; invalid < 4; ++invalid) {
        SCOPED_TRACE(invalid);
        Harness<> h;
        h.manual(order(1));
        auto batch = plan({order(2)}, 3);
        batch.cancel_order_ids = {1};
        if (invalid == 0) batch.cancel_order_ids = {1, 1};
        if (invalid == 1) batch.cancel_order_ids = {99};
        if (invalid == 2) {
            ASSERT_TRUE(h.engine.processExchangeEvent(OrderUpdate{1, kOpen, ExecutionOrderStatus::Rejected}, h.persist()));
        }
        if (invalid == 3) h.engine.orderManager().markCancelRequested(1, kOpen);
        const auto committed = h.saved.size();
        EXPECT_THROW(h.engine.applyOrderPlan(batch, h.persist()), std::invalid_argument);
        EXPECT_EQ(h.saved.size(), committed);
        EXPECT_EQ(h.exchange.submitCount(), 1U);
        EXPECT_EQ(h.exchange.cancelRequestCount(1), 0U);
        EXPECT_EQ(h.engine.nextOrderId(), 2U);
        EXPECT_EQ(h.engine.lastExecutionTimestamp(), 0U);
        EXPECT_EQ(h.engine.orderManager().orders().size(), 1U);
    }
}

TEST(ExecutionEngine, R5_MalformedMutableOrderDtoCannotBypassSubmissionValidation)
{
    for (int invalid = 0; invalid < 5; ++invalid) {
        SCOPED_TRACE(invalid);
        Harness<> h;
        auto request = order(1);
        if (invalid == 0) request.quantity = 0.0;
        if (invalid == 1) request.quantity = -1.0;
        if (invalid == 2) request.quantity = std::numeric_limits<double>::quiet_NaN();
        if (invalid == 3) request.quantity = std::numeric_limits<double>::infinity();
        if (invalid == 4) request.coin.clear();
        EXPECT_THROW(h.engine.applyOrderPlan(plan({request}, 2), h.persist()), std::invalid_argument);
        EXPECT_TRUE(h.saved.empty());
        EXPECT_EQ(h.exchange.submitCount(), 0U);
        EXPECT_TRUE(h.engine.orderManager().orders().empty());
        EXPECT_EQ(h.engine.nextOrderId(), 1U);
        EXPECT_EQ(h.engine.lastExecutionTimestamp(), 0U);
    }
}

TEST(ExecutionEngine, R12_NonfiniteGlobalTargetCannotPartiallyAdvanceCycleState)
{
    for (const double invalid : {std::numeric_limits<double>::quiet_NaN(),
             std::numeric_limits<double>::infinity()}) {
        Harness<> h;
        auto initial = plan({}, 1);
        initial.global_target_exposure["BTC"] = 100.0;
        h.engine.applyOrderPlan(initial, h.persist());
        const auto committed = h.saved.size();
        auto bad = plan({order(1)}, 2);
        bad.execution_timestamp = kLater;
        bad.global_target_exposure["BTC"] = invalid;
        EXPECT_THROW(h.engine.applyOrderPlan(bad, h.persist()), std::invalid_argument);
        EXPECT_EQ(h.saved.size(), committed);
        EXPECT_EQ(h.exchange.submitCount(), 0U);
        EXPECT_TRUE(h.engine.orderManager().orders().empty());
        EXPECT_EQ(h.engine.nextOrderId(), 1U);
        EXPECT_EQ(h.engine.lastExecutionTimestamp(), kOpen);
        EXPECT_DOUBLE_EQ(h.engine.lastGlobalTarget().get("BTC"), 100.0);
    }
}

TEST(ExecutionEngine, R13_PlanCommitFailureLeavesCreatedIntentWithoutDispatchOrMoney)
{
    Harness<> h;
    EXPECT_THROW(h.engine.applyOrderPlan(plan({order(1), order(2)}, 3),
        [&](const std::optional<Fill>& incoming) {
            EXPECT_FALSE(incoming.has_value());
            EXPECT_EQ(h.exchange.submitCount(), 0U);
            EXPECT_EQ(h.engine.orderManager().orders().size(), 2U);
            throw std::runtime_error("Injected plan commit failure");
        }), std::runtime_error);
    EXPECT_EQ(h.exchange.submitCount(), 0U);
    ASSERT_NE(h.engine.orderManager().find(1), nullptr);
    ASSERT_NE(h.engine.orderManager().find(2), nullptr);
    EXPECT_EQ(h.engine.orderManager().find(1)->status, ExecutionOrderStatus::Created);
    EXPECT_EQ(h.engine.orderManager().find(2)->status, ExecutionOrderStatus::Created);
    EXPECT_DOUBLE_EQ(h.account.cash(), 1000.0);
    EXPECT_TRUE(h.account.positions().values().empty());
}

TEST(ExecutionEngine, R13_MidDispatchFailureRetainsWholeRecoverablePlan)
{
    Harness<ObservedExchange> h;
    h.exchange.fail_submit_id = 2;
    EXPECT_THROW(h.engine.applyOrderPlan(plan({order(1), order(2)}, 3), h.persist()), std::runtime_error);
    ASSERT_EQ(h.exchange.commands.size(), 1U);
    EXPECT_TRUE(h.exchange.commands.front().submit);
    EXPECT_EQ(h.exchange.commands.front().id, 1U);
    ASSERT_FALSE(h.saved.empty());
    const auto& committed = h.saved.back();
    EXPECT_EQ(committed.next_id, 3U);
    ASSERT_EQ(committed.orders.size(), 2U);
    OrderManager restored;
    restored.restore(committed.orders, committed.fill_ids);
    EXPECT_EQ(restored.find(1)->status, ExecutionOrderStatus::Submitted);
    EXPECT_EQ(restored.find(2)->status, ExecutionOrderStatus::Created);
    EXPECT_DOUBLE_EQ(committed.cash, 1000.0);
    EXPECT_TRUE(committed.account_positions.empty());
}

TEST(ExecutionEngine, R14_RepeatedTargetUsesNextOpenAndExistingPartialRemainder)
{
    Harness<> h;
    const auto prices = marks(125.0);
    const auto decisions = target(0.5);
    h.engine.executeDecisionBatch(kOpen, prices, decisions, h.persist());
    ASSERT_NE(h.engine.orderManager().find(1), nullptr);
    EXPECT_DOUBLE_EQ(h.engine.orderManager().find(1)->request.quantity, 4.0);
    EXPECT_DOUBLE_EQ(h.account.cash(), 1000.0);
    h.exchange.acceptOrder(1, kOpen);
    h.exchange.fillOrder(1, 1.0, 125.0, kOpen);
    h.engine.processExchangeEvents(h.persist());
    h.engine.executeDecisionBatch(kOpen, prices, decisions, h.persist());
    EXPECT_EQ(h.exchange.submitCount(), 1U);
    EXPECT_EQ(h.exchange.cancelRequestCount(1), 0U);
    EXPECT_DOUBLE_EQ(h.engine.strategyPosition(kAlpha).get("BTC"), 1.0);
    EXPECT_DOUBLE_EQ(h.engine.orderManager().pendingSignedQuantity(kAlpha, "BTC"), 3.0);
    EXPECT_DOUBLE_EQ(h.account.cash(), 873.75);
    EXPECT_EQ(h.engine.nextOrderId(), 2U);
}

TEST(ExecutionEngine, R14_ChangedTargetCancelsOnceAndWaitsForVenueConfirmation)
{
    Harness<> h;
    const auto prices = marks(125.0);
    h.engine.executeDecisionBatch(kOpen, prices, target(0.5), h.persist());
    h.exchange.acceptOrder(1, kOpen);
    h.exchange.fillOrder(1, 1.0, 125.0, kOpen);
    h.engine.processExchangeEvents(h.persist());
    h.engine.executeDecisionBatch(kOpen, prices, target(0.125), h.persist());
    h.engine.executeDecisionBatch(kOpen, prices, target(0.125), h.persist());
    EXPECT_EQ(h.exchange.cancelRequestCount(1), 1U);
    EXPECT_EQ(h.exchange.submitCount(), 1U);
    EXPECT_DOUBLE_EQ(h.engine.orderManager().pendingSignedQuantity(kAlpha, "BTC"), 3.0);
    h.exchange.confirmCancel(1, kOpen);
    h.engine.processExchangeEvents(h.persist());
    h.engine.executeDecisionBatch(kLater, prices, target(0.25), h.persist());
    EXPECT_EQ(h.exchange.submitCount(), 2U);
    ASSERT_NE(h.engine.orderManager().find(2), nullptr);
    EXPECT_EQ(h.engine.orderManager().find(2)->request.side, OrderSide::Buy);
    EXPECT_DOUBLE_EQ(h.engine.orderManager().find(2)->request.quantity, 1.0);
    EXPECT_DOUBLE_EQ(h.engine.strategyPosition(kAlpha).get("BTC"), 1.0);
}

TEST(ExecutionEngine, R14_FlatSellsActualFilledQuantityDespitePriceGap)
{
    Harness<> h;
    h.manual(order(1, OrderSide::Buy, 1.0));
    h.exchange.acceptOrder(1, kOpen);
    h.exchange.fillOrder(1, 1.0, 125.0, kOpen);
    h.engine.processExchangeEvents(h.persist());
    h.engine.executeDecisionBatch(kLater, marks(250.0), target(0.0), h.persist());
    ASSERT_NE(h.engine.orderManager().find(2), nullptr);
    EXPECT_EQ(h.engine.orderManager().find(2)->request.side, OrderSide::Sell);
    EXPECT_DOUBLE_EQ(h.engine.orderManager().find(2)->request.quantity, 1.0);
    EXPECT_DOUBLE_EQ(h.account.cash(), 873.75);
    EXPECT_DOUBLE_EQ(h.account.positions().get("BTC"), 1.0);
}

TEST(ExecutionEngine, R15_ManualOrderStagesBeforeDispatchAndPreservesDailyMarkers)
{
    Harness<> h;
    h.engine.applyOrderPlan(plan({}, 1), h.persist());
    bool observedDurableIntent = false;
    h.engine.applyManualOrders({order(1, OrderSide::Buy, 2.0)}, 2, kLater,
        [&](const std::optional<Fill>& incoming) {
            if (!observedDurableIntent) {
                observedDurableIntent = true;
                EXPECT_EQ(h.exchange.submitCount(), 0U);
                ASSERT_NE(h.engine.orderManager().find(1), nullptr);
                EXPECT_EQ(h.engine.orderManager().find(1)->status, ExecutionOrderStatus::Created);
                EXPECT_EQ(h.engine.nextOrderId(), 2U);
            }
            EXPECT_EQ(h.engine.lastExecutionTimestamp(), kOpen);
            EXPECT_DOUBLE_EQ(h.engine.lastGlobalTarget().get("BTC"), 0.0);
            h.capture(incoming);
        });
    EXPECT_TRUE(observedDurableIntent);
    EXPECT_EQ(h.exchange.submitCount(), 1U);
    EXPECT_EQ(h.engine.lastExecutionTimestamp(), kOpen);
    EXPECT_DOUBLE_EQ(h.account.cash(), 1000.0);
}

TEST(ExecutionEngine, R15_InvalidManualCommandsCannotStageOrDispatch)
{
    for (int invalid = 0; invalid < 4; ++invalid) {
        Harness<> h;
        auto request = order(1);
        OrderID nextId = 2;
        Timestamp at = kLater;
        if (invalid == 0) at = 0;
        if (invalid == 1) nextId = 0;
        if (invalid == 2) nextId = 1;
        if (invalid == 3) request.strategy_id = 99;
        EXPECT_THROW(h.engine.applyManualOrders({request}, nextId, at, h.persist()), std::invalid_argument);
        EXPECT_TRUE(h.saved.empty());
        EXPECT_EQ(h.exchange.submitCount(), 0U);
        EXPECT_TRUE(h.engine.orderManager().orders().empty());
        EXPECT_EQ(h.engine.nextOrderId(), 1U);
    }
}

TEST(ExecutionEngine, R16_RestoreRetainsPartialExposureAndFillIdWithoutSendingCommands)
{
    Harness<> original;
    original.manual(order());
    original.exchange.acceptOrder(10, kOpen);
    original.exchange.fillOrder(10, 2.0, 100.0, kOpen);
    original.engine.processExchangeEvents(original.persist());
    ASSERT_FALSE(original.saved.empty());
    auto saved = original.saved.back();
    saved.last_execution = kOpen;
    saved.strategy_positions.erase(kBeta); // Missing configured strategy restores flat.
    Harness<> restarted;
    restarted.restore(saved);
    EXPECT_EQ(restarted.exchange.submitCount(), 0U);
    EXPECT_EQ(restarted.exchange.activeOrderCount(), 0U);
    EXPECT_EQ(restarted.engine.nextOrderId(), 11U);
    EXPECT_EQ(restarted.engine.lastExecutionTimestamp(), kOpen);
    EXPECT_DOUBLE_EQ(restarted.engine.strategyPosition(kBeta).get("BTC"), 0.0);
    EXPECT_DOUBLE_EQ(restarted.engine.orderManager().pendingSignedQuantity(kAlpha, "BTC"), 3.0);
    ASSERT_EQ(saved.fill_ids.size(), 1U);
    const auto duplicate = fill(order(), 2.0, 100.0, 2.0, saved.fill_ids.front());
    ASSERT_TRUE(restarted.engine.processExchangeEvent(duplicate, restarted.persist()));
    EXPECT_TRUE(restarted.saved.empty());
    EXPECT_DOUBLE_EQ(restarted.account.cash(), 798.0);
    ASSERT_TRUE(restarted.engine.processExchangeEvent(fill(order(), 3.0, 110.0, 3.3, 102), restarted.persist()));
    EXPECT_DOUBLE_EQ(restarted.account.cash(), 464.70);
    EXPECT_DOUBLE_EQ(restarted.account.positions().get("BTC"), 5.0);
    EXPECT_DOUBLE_EQ(restarted.engine.strategyPosition(kAlpha).get("BTC"), 5.0);
}

TEST(ExecutionEngine, R16_InvalidRecoveryIdentityIsRejectedWithoutExchangeEffects)
{
    for (int invalid = 0; invalid < 3; ++invalid) {
        Harness<> h;
        StrategyPositionSnapshot positions;
        std::vector<TrackedOrder> orders;
        OrderID nextId = 11;
        if (invalid == 0) nextId = 0;
        if (invalid == 1) {
            orders.emplace_back(order());
            nextId = 10;
        }
        if (invalid == 2) positions[99].set("BTC", 2.0);
        EXPECT_THROW(h.engine.restoreState(798.0, {{"BTC", 2.0}}, positions,
            orders, {101}, nextId, kOpen), std::invalid_argument);
        EXPECT_EQ(h.exchange.submitCount(), 0U);
        EXPECT_TRUE(h.saved.empty());
        EXPECT_DOUBLE_EQ(h.account.cash(), 1000.0);
        EXPECT_EQ(h.engine.nextOrderId(), 1U);
    }
}

} // namespace
