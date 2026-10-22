#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <vector>

#include "execution_test_support.h"

namespace {
using namespace execution_test;

TEST(OrderManager, R2_PartialFillsReduceSignedLongAndShortPending)
{
    for (const auto side : {OrderSide::Buy, OrderSide::Sell}) {
        OrderManager manager;
        const auto request = order(10, side);
        submit(manager, request);
        ASSERT_TRUE(manager.onOrderUpdate({10, kOpen, ExecutionOrderStatus::Accepted}));
        ASSERT_TRUE(manager.onFill(fill(request)));
        ASSERT_NE(manager.find(10), nullptr);
        EXPECT_EQ(manager.find(10)->status, ExecutionOrderStatus::PartiallyFilled);
        EXPECT_DOUBLE_EQ(manager.find(10)->filled_quantity, 2.0);
        EXPECT_DOUBLE_EQ(manager.find(10)->remainingQuantity(), 3.0);
        EXPECT_DOUBLE_EQ(manager.pendingSignedQuantity(kAlpha, "BTC"), side == OrderSide::Buy ? 3.0 : -3.0);
        ASSERT_TRUE(manager.onFill(fill(request, 3.0, 110.0, 3.3, 102)));
        EXPECT_EQ(manager.find(10)->status, ExecutionOrderStatus::Filled);
        EXPECT_DOUBLE_EQ(manager.find(10)->filled_quantity, 5.0);
        EXPECT_DOUBLE_EQ(manager.pendingSignedQuantity(kAlpha, "BTC"), 0.0);
        EXPECT_FALSE(manager.hasOpenOrder(kAlpha, "BTC"));
    }
}

TEST(OrderManager, R2_TerminalResponsesRemovePendingWithoutInventingFills)
{
    for (const auto status : {ExecutionOrderStatus::Filled, ExecutionOrderStatus::Canceled,
                             ExecutionOrderStatus::Rejected}) {
        OrderManager manager;
        submit(manager, order());
        ASSERT_TRUE(manager.onOrderUpdate({10, kOpen, status}));
        ASSERT_NE(manager.find(10), nullptr);
        EXPECT_DOUBLE_EQ(manager.find(10)->filled_quantity, 0.0);
        EXPECT_DOUBLE_EQ(manager.pendingSignedQuantity(kAlpha, "BTC"), 0.0);
        EXPECT_TRUE(manager.openOrderIds().empty());
    }
}

TEST(OrderManager, R3_CancelRequestRetainsRemainderUntilAcknowledged)
{
    OrderManager manager;
    const auto request = order();
    submit(manager, request);
    ASSERT_TRUE(manager.onFill(fill(request)));
    manager.markCancelRequested(10, kOpen);
    EXPECT_DOUBLE_EQ(manager.pendingSignedQuantity(kAlpha, "BTC"), 3.0);
    EXPECT_TRUE(manager.cancelableOpenOrderIds(kAlpha, "BTC").empty());
    EXPECT_TRUE(manager.hasOpenOrder(kAlpha, "BTC"));
    ASSERT_TRUE(manager.onOrderUpdate({10, kLater, ExecutionOrderStatus::Canceled}));
    EXPECT_DOUBLE_EQ(manager.find(10)->filled_quantity, 2.0);
    EXPECT_DOUBLE_EQ(manager.pendingSignedQuantity(kAlpha, "BTC"), 0.0);
}

TEST(OrderManager, R3_LatePartialFillDoesNotReopenCanceledRemainder)
{
    OrderManager manager;
    const auto request = order();
    submit(manager, request);
    ASSERT_TRUE(manager.onFill(fill(request)));
    ASSERT_TRUE(manager.onOrderUpdate({10, kOpen, ExecutionOrderStatus::Canceled}));
    ASSERT_TRUE(manager.onFill(fill(request, 1.0, 110.0, 1.1, 102)));
    EXPECT_EQ(manager.find(10)->status, ExecutionOrderStatus::Canceled);
    EXPECT_DOUBLE_EQ(manager.find(10)->filled_quantity, 3.0);
    EXPECT_DOUBLE_EQ(manager.find(10)->remainingQuantity(), 2.0);
    EXPECT_DOUBLE_EQ(manager.pendingSignedQuantity(kAlpha, "BTC"), 0.0);
}

TEST(OrderManager, R3_LateCompletionCanCloseEntireCanceledRequest)
{
    OrderManager manager;
    const auto request = order();
    submit(manager, request);
    ASSERT_TRUE(manager.onFill(fill(request)));
    ASSERT_TRUE(manager.onOrderUpdate({10, kOpen, ExecutionOrderStatus::Canceled}));
    ASSERT_TRUE(manager.onFill(fill(request, 3.0, 110.0, 3.3, 102)));
    EXPECT_EQ(manager.find(10)->status, ExecutionOrderStatus::Filled);
    EXPECT_DOUBLE_EQ(manager.find(10)->filled_quantity, 5.0);
    EXPECT_FALSE(manager.hasOpenOrder(kAlpha, "BTC"));
}

TEST(OrderManager, R4_DelayedAcceptedEchoPreservesPartialStateAndTimestamp)
{
    OrderManager manager;
    const auto request = order();
    submit(manager, request);
    ASSERT_TRUE(manager.onFill(fill(request)));
    EXPECT_FALSE(manager.onOrderUpdate({10, kLater, ExecutionOrderStatus::Accepted, "stale-venue-id"}));
    EXPECT_EQ(manager.find(10)->status, ExecutionOrderStatus::PartiallyFilled);
    EXPECT_EQ(manager.find(10)->updated_at, kOpen);
    EXPECT_DOUBLE_EQ(manager.find(10)->filled_quantity, 2.0);
    EXPECT_TRUE(manager.find(10)->exchange_order_id.empty());
}

TEST(OrderManager, R4_TerminalOrdersIgnoreEarlierLifecycleEchoes)
{
    OrderManager manager;
    const auto request = order();
    submit(manager, request);
    ASSERT_TRUE(manager.onFill(fill(request, 5.0, 100.0, 5.0)));
    for (const auto echo : {ExecutionOrderStatus::Submitted, ExecutionOrderStatus::Accepted,
                           ExecutionOrderStatus::PartiallyFilled}) {
        EXPECT_FALSE(manager.onOrderUpdate({10, kLater, echo}));
        EXPECT_EQ(manager.find(10)->status, ExecutionOrderStatus::Filled);
        EXPECT_EQ(manager.find(10)->updated_at, kOpen);
        EXPECT_DOUBLE_EQ(manager.find(10)->filled_quantity, 5.0);
    }
    EXPECT_TRUE(manager.onOrderUpdate({10, kLater, ExecutionOrderStatus::Filled}));
    EXPECT_DOUBLE_EQ(manager.find(10)->filled_quantity, 5.0);
}

TEST(OrderManager, R4_OlderSameStageEchoCannotRegressBusinessTimeOrIdentity)
{
    OrderManager manager;
    submit(manager, order());
    ASSERT_TRUE(manager.onOrderUpdate({10, kOpen, ExecutionOrderStatus::Accepted, "current-id"}));
    EXPECT_FALSE(manager.onOrderUpdate({10, kClose, ExecutionOrderStatus::Accepted, "older-id"}));
    EXPECT_EQ(manager.find(10)->updated_at, kOpen);
    EXPECT_EQ(manager.find(10)->exchange_order_id, "current-id");
}

TEST(OrderManager, R2_ContradictoryTerminalTransitionsAreRejected)
{
    OrderManager manager;
    submit(manager, order());
    ASSERT_TRUE(manager.onOrderUpdate({10, kOpen, ExecutionOrderStatus::Filled}));
    EXPECT_THROW(manager.onOrderUpdate({10, kLater, ExecutionOrderStatus::Rejected}), std::logic_error);
    EXPECT_THROW(manager.onOrderUpdate({10, kLater, ExecutionOrderStatus::Canceled}), std::logic_error);
    EXPECT_THROW(manager.markCancelRequested(10, kLater), std::logic_error);
    EXPECT_EQ(manager.find(10)->status, ExecutionOrderStatus::Filled);
    EXPECT_EQ(manager.find(10)->updated_at, kOpen);
}

TEST(OrderManager, R5_DuplicateAndZeroOrderIdsCannotReplaceLiveOrders)
{
    OrderManager manager;
    submit(manager, order());
    EXPECT_THROW(manager.track(order(10, OrderSide::Sell, 2.0)), std::invalid_argument);
    EXPECT_THROW(manager.track(order(0)), std::invalid_argument);
    ASSERT_EQ(manager.orders().size(), 1U);
    EXPECT_EQ(manager.find(10)->request.side, OrderSide::Buy);
    EXPECT_DOUBLE_EQ(manager.pendingSignedQuantity(kAlpha, "BTC"), 5.0);
}

TEST(OrderManager, R6_UnknownOrderUpdatesCancelsAndFillsAreRejected)
{
    OrderManager manager;
    EXPECT_THROW(manager.onOrderUpdate({10, kOpen, ExecutionOrderStatus::Accepted}), std::invalid_argument);
    EXPECT_THROW(manager.markCancelRequested(10, kOpen), std::invalid_argument);
    EXPECT_THROW(manager.onFill(fill(order())), std::invalid_argument);
    EXPECT_TRUE(manager.orders().empty());
}

TEST(OrderManager, R6_MismatchedStrategyAssetOrSideCannotFillOrder)
{
    for (int mismatch = 0; mismatch < 3; ++mismatch) {
        SCOPED_TRACE(mismatch);
        OrderManager manager;
        submit(manager, order());
        auto bad = fill(order());
        if (mismatch == 0) bad.strategy_id = kBeta;
        if (mismatch == 1) bad.coin = "ETH";
        if (mismatch == 2) bad.side = OrderSide::Sell;
        EXPECT_THROW(manager.onFill(bad), std::logic_error);
        EXPECT_DOUBLE_EQ(manager.find(10)->filled_quantity, 0.0);
        EXPECT_DOUBLE_EQ(manager.pendingSignedQuantity(kAlpha, "BTC"), 5.0);
    }
}

TEST(OrderManager, R6_OverfillAndFillOnRejectedOrderDoNotChangeQuantity)
{
    OrderManager manager;
    submit(manager, order());
    ASSERT_TRUE(manager.onFill(fill(order())));
    EXPECT_THROW(manager.onFill(fill(order(), 4.0, 100.0, 4.0, 102)), std::logic_error);
    EXPECT_DOUBLE_EQ(manager.find(10)->filled_quantity, 2.0);
    EXPECT_DOUBLE_EQ(manager.pendingSignedQuantity(kAlpha, "BTC"), 3.0);
    submit(manager, order(11));
    ASSERT_TRUE(manager.onOrderUpdate({11, kOpen, ExecutionOrderStatus::Rejected}));
    EXPECT_THROW(manager.onFill(fill(order(11), 2.0, 100.0, 2.0, 103)), std::logic_error);
    EXPECT_DOUBLE_EQ(manager.find(11)->filled_quantity, 0.0);
}

// LIVE (execution-state --venue-lot-overfill yes): the venue rounds to its nearest lot, so a
// fill may exceed the order, up to twice it. Default managers keep R6.
TEST(OrderManager, R6_VenueLotOverfillAcceptedOnlyWhenAllowed)
{
    OrderManager manager;
    manager.allowVenueLotOverfill(2.0);
    submit(manager, order());                                    // 5 requested
    ASSERT_TRUE(manager.onFill(fill(order(), 6.0, 100.0, 6.0, 101)));
    EXPECT_DOUBLE_EQ(manager.find(10)->filled_quantity, 6.0);
    EXPECT_EQ(manager.find(10)->status, ExecutionOrderStatus::Filled);
    EXPECT_DOUBLE_EQ(manager.pendingSignedQuantity(kAlpha, "BTC"), 0.0);

    submit(manager, order(11));
    EXPECT_THROW(manager.onFill(fill(order(11), 10.5, 100.0, 10.5, 102)), std::logic_error); // > 2x
    EXPECT_THROW(manager.allowVenueLotOverfill(2.5), std::invalid_argument);

    // Restored state keeps the same limit.
    OrderManager restored;
    restored.allowVenueLotOverfill(2.0);
    restored.restore({*manager.find(10)}, {101});
    EXPECT_DOUBLE_EQ(restored.find(10)->filled_quantity, 6.0);
    EXPECT_THROW(OrderManager().restore({*manager.find(10)}, {101}), std::invalid_argument);
}

TEST(OrderManager, R7_DuplicateFillIdDoesNotConsumeRemainingQuantity)
{
    OrderManager manager;
    const auto incoming = fill(order());
    submit(manager, order());
    ASSERT_TRUE(manager.onFill(incoming));
    EXPECT_FALSE(manager.onFill(incoming));
    EXPECT_DOUBLE_EQ(manager.find(10)->filled_quantity, 2.0);
    EXPECT_DOUBLE_EQ(manager.pendingSignedQuantity(kAlpha, "BTC"), 3.0);
    EXPECT_EQ(manager.processedFillIds(), std::vector<FillID>{101});
}

TEST(OrderManager, R7_RejectedMismatchedFillDoesNotPoisonCorrectedFillId)
{
    for (int mismatch = 0; mismatch < 3; ++mismatch) {
        SCOPED_TRACE(mismatch);
        OrderManager manager;
        submit(manager, order());
        auto bad = fill(order());
        if (mismatch == 0) bad.coin = "ETH";
        if (mismatch == 1) bad.strategy_id = kBeta;
        if (mismatch == 2) bad.side = OrderSide::Sell;
        EXPECT_THROW(manager.onFill(bad), std::logic_error);
        EXPECT_TRUE(manager.processedFillIds().empty());
        EXPECT_TRUE(manager.onFill(fill(order())));
        EXPECT_DOUBLE_EQ(manager.find(10)->filled_quantity, 2.0);
    }
}

TEST(OrderManager, R7_UnknownRejectedAndOverlargeAttemptsDoNotConsumeFillIds)
{
    for (int invalid = 0; invalid < 3; ++invalid) {
        SCOPED_TRACE(invalid);
        OrderManager manager;
        if (invalid != 0) submit(manager, order());
        if (invalid == 1) {
            ASSERT_TRUE(manager.onOrderUpdate({10, kOpen, ExecutionOrderStatus::Rejected}));
        }
        if (invalid == 0)
            EXPECT_THROW(manager.onFill(fill(order())), std::invalid_argument);
        else if (invalid == 1)
            EXPECT_THROW(manager.onFill(fill(order())), std::logic_error);
        else
            EXPECT_THROW(manager.onFill(fill(order(), 6.0)), std::logic_error);
        EXPECT_TRUE(manager.processedFillIds().empty());
    }
}

TEST(OrderManager, R16_RestoredPartialOrderRetainsDeduplicationAndRemainder)
{
    OrderManager before;
    submit(before, order());
    ASSERT_TRUE(before.onFill(fill(order())));
    OrderManager restarted;
    restarted.restore(trackedOrders(before), before.processedFillIds());
    EXPECT_FALSE(restarted.onFill(fill(order())));
    EXPECT_DOUBLE_EQ(restarted.pendingSignedQuantity(kAlpha, "BTC"), 3.0);
    EXPECT_TRUE(restarted.onFill(fill(order(), 3.0, 110.0, 3.3, 102)));
    EXPECT_DOUBLE_EQ(restarted.find(10)->filled_quantity, 5.0);
    EXPECT_EQ(restarted.find(10)->status, ExecutionOrderStatus::Filled);
}

TEST(OrderManager, R16_InvalidRestorationPreservesPriorValidState)
{
    OrderManager manager;
    submit(manager, order());
    ASSERT_TRUE(manager.onFill(fill(order())));
    const auto baseline = trackedOrders(manager);
    for (int invalid = 0; invalid < 7; ++invalid) {
        SCOPED_TRACE(invalid);
        auto orders = baseline;
        std::vector<FillID> ids{101};
        if (invalid == 0) orders.push_back(orders.front());
        if (invalid == 1) orders.front().request.order_id = 0;
        if (invalid == 2) orders.front().filled_quantity = -1.0;
        if (invalid == 3) orders.front().filled_quantity = 6.0;
        if (invalid == 4) orders.front().filled_quantity = std::numeric_limits<double>::quiet_NaN();
        if (invalid == 5) ids = {101, 101};
        if (invalid == 6) ids = {0};
        EXPECT_THROW(manager.restore(orders, ids), std::invalid_argument);
        EXPECT_DOUBLE_EQ(manager.find(10)->filled_quantity, 2.0);
        EXPECT_DOUBLE_EQ(manager.pendingSignedQuantity(kAlpha, "BTC"), 3.0);
        EXPECT_EQ(manager.processedFillIds(), std::vector<FillID>{101});
    }
}

TEST(OrderManager, R9_PendingQuantitiesRemainAttributedToStrategyAndAsset)
{
    OrderManager manager;
    submit(manager, order(10, OrderSide::Buy, 5.0, kAlpha));
    submit(manager, order(11, OrderSide::Sell, 2.0, kBeta));
    auto eth = order(12, OrderSide::Sell, 3.0, kAlpha);
    eth.coin = "ETH";
    submit(manager, eth);
    EXPECT_DOUBLE_EQ(manager.pendingSignedQuantity(kAlpha, "BTC"), 5.0);
    EXPECT_DOUBLE_EQ(manager.pendingSignedQuantity(kBeta, "BTC"), -2.0);
    EXPECT_DOUBLE_EQ(manager.pendingSignedQuantity(kAlpha, "ETH"), -3.0);
    EXPECT_EQ(manager.openOrderIds(kAlpha, "BTC"), std::vector<OrderID>{10});
}

TEST(OrderManager, R19_ProtectiveOrdersAreOpenWithoutDirectionalPendingExposure)
{
    OrderManager manager;
    auto liveStop = order(10, OrderSide::Sell, 2.0);
    liveStop.position_stop_price = 90.0;
    auto cover = order(11, OrderSide::Buy, 2.0);
    cover.parent_order_id = 8;
    cover.protective_stop_price = 120.0;
    submit(manager, liveStop);
    submit(manager, cover);
    EXPECT_DOUBLE_EQ(manager.pendingSignedQuantity(kAlpha, "BTC"), 0.0);
    EXPECT_EQ(manager.openOrderIds().size(), 2U);
}

} // namespace
