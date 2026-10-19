#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>
#include <vector>

#include "planning_test_support.h"

namespace {
using namespace planning_test;

TEST(QuantityBuilder, R13_DeltasFlattenAndCrossZeroWithCallerTiming)
{
    struct Example { double current; double target; OrderSide side; double order; };
    const std::vector<Example> examples{
        {2.0, 5.0, OrderSide::Buy, 3.0},
        {2.0, -3.0, OrderSide::Sell, 5.0},
        {3.0, 0.0, OrderSide::Sell, 3.0},
        {-3.0, 0.0, OrderSide::Buy, 3.0},
    };
    for (const auto& example : examples) {
        SCOPED_TRACE(example.target);
        OrderID nextId = 40;
        const auto result = QuantityOrderBuilder().createMarketPlan(
            {quantityTarget(example.target)}, {held(example.current)}, OrderManager{},
            kClose, kOpen, nextId);
        EXPECT_TRUE(result.order_ids_to_cancel.empty());
        ASSERT_EQ(result.orders_to_submit.size(), 1U);
        const auto& order = result.orders_to_submit.front();
        EXPECT_EQ(order.side, example.side);
        EXPECT_DOUBLE_EQ(order.quantity, example.order);
        EXPECT_EQ(order.strategy_id, kStrategy);
        EXPECT_EQ(order.coin, "BTC");
        EXPECT_EQ(order.created_at, kClose);
        EXPECT_EQ(order.active_from, kOpen);
        EXPECT_EQ(order.order_id, 40U);
        EXPECT_EQ(nextId, 41U);
    }
}

TEST(QuantityBuilder, R13_RejectsMisalignedStrategyVectors)
{
    OrderID nextId = 40;
    EXPECT_THROW(QuantityOrderBuilder().createMarketPlan({quantityTarget(5.0)}, {},
        OrderManager{}, kClose, kOpen, nextId), std::invalid_argument);
    EXPECT_THROW(QuantityOrderBuilder().createMarketPlan({}, {held(2.0)},
        OrderManager{}, kClose, kOpen, nextId), std::invalid_argument);
    EXPECT_EQ(nextId, 40U);
}

TEST(QuantityBuilder, R6_UndercoveredPendingOrderIsCanceledBeforeLargerReplacement)
{
    OrderManager orders;
    orders.restore({pending(OrderSide::Buy, 2.0, 0.0)}, {});
    OrderID nextId = 40;
    const auto result = QuantityOrderBuilder().createMarketPlan({quantityTarget(5.0)},
        {held(0.0)}, orders, kClose, kOpen, nextId);
    EXPECT_EQ(result.order_ids_to_cancel, std::vector<OrderID>{10});
    EXPECT_TRUE(result.orders_to_submit.empty());
    EXPECT_EQ(nextId, 40U);
}

TEST(QuantityBuilder, R6_OrphanPendingAssetIsCanceledEvenWithoutTargetOrPosition)
{
    OrderManager orders;
    orders.restore({pending(OrderSide::Buy, 2.0, 0.0)}, {});
    OrderID nextId = 40;
    const auto result = QuantityOrderBuilder().createMarketPlan({quantityTarget(0.0)},
        {held(0.0)}, orders, kClose, kOpen, nextId);
    EXPECT_EQ(result.order_ids_to_cancel, std::vector<OrderID>{10});
    EXPECT_TRUE(result.orders_to_submit.empty());
    EXPECT_EQ(nextId, 40U);
}

TEST(QuantityBuilder, R5_TerminalOrdersDoNotSuppressNeededQuantity)
{
    for (const auto status : {ExecutionOrderStatus::Filled, ExecutionOrderStatus::Canceled,
                             ExecutionOrderStatus::Rejected}) {
        SCOPED_TRACE(static_cast<int>(status));
        auto old = pending(OrderSide::Buy, 5.0, 0.0);
        old.status = status;
        if (status == ExecutionOrderStatus::Filled)
            old.filled_quantity = 5.0;
        OrderManager orders;
        orders.restore({old}, {});
        OrderID nextId = 40;
        // Current authoritative position is 2, regardless of old order history.
        const auto result = QuantityOrderBuilder().createMarketPlan({quantityTarget(5.0)},
            {held(2.0)}, orders, kClose, kOpen, nextId);
        EXPECT_TRUE(result.order_ids_to_cancel.empty());
        ASSERT_EQ(result.orders_to_submit.size(), 1U);
        EXPECT_EQ(result.orders_to_submit.front().side, OrderSide::Buy);
        EXPECT_DOUBLE_EQ(result.orders_to_submit.front().quantity, 3.0);
    }
}

TEST(OrderTargetGuard, R14_LongRemainingDeltaAllowsSmallerBuyButNoOvershootOrSell)
{
    const OrderTargetGuard guard;
    const ExecutionOrder buyTwo(40, kStrategy, kClose, kOpen, "BTC", OrderSide::Buy, 2.0);
    const ExecutionOrder buyOne(41, kStrategy, kClose, kOpen, "BTC", OrderSide::Buy, 1.0);
    const ExecutionOrder buyThree(42, kStrategy, kClose, kOpen, "BTC", OrderSide::Buy, 3.0);
    const ExecutionOrder sellOne(43, kStrategy, kClose, kOpen, "BTC", OrderSide::Sell, 1.0);
    EXPECT_TRUE(guard.allows(buyTwo, 5.0, 2.0, 1.0));
    EXPECT_TRUE(guard.allows(buyOne, 5.0, 2.0, 1.0));
    EXPECT_FALSE(guard.allows(buyThree, 5.0, 2.0, 1.0));
    EXPECT_FALSE(guard.allows(sellOne, 5.0, 2.0, 1.0));
    EXPECT_FALSE(guard.allows(buyOne, 3.0, 2.0, 1.0));
    EXPECT_THROW(guard.validate(buyThree, 5.0, 2.0, 1.0), std::logic_error);
}

TEST(OrderTargetGuard, R14_ShortRemainingDeltaUsesSignedPendingAndSellDirection)
{
    const OrderTargetGuard guard;
    const ExecutionOrder sellTwo(40, kStrategy, kClose, kOpen, "BTC", OrderSide::Sell, 2.0);
    const ExecutionOrder sellThree(41, kStrategy, kClose, kOpen, "BTC", OrderSide::Sell, 3.0);
    const ExecutionOrder buyOne(42, kStrategy, kClose, kOpen, "BTC", OrderSide::Buy, 1.0);
    EXPECT_TRUE(guard.allows(sellTwo, -5.0, -2.0, -1.0));
    EXPECT_FALSE(guard.allows(sellThree, -5.0, -2.0, -1.0));
    EXPECT_FALSE(guard.allows(buyOne, -5.0, -2.0, -1.0));
    EXPECT_FALSE(guard.allows(sellTwo, -3.0, -2.0, -1.0));
}

TEST(OrderTargetGuard, R14_NonfinitePlanningStateNeverAuthorizesOrder)
{
    const OrderTargetGuard guard;
    const ExecutionOrder buy(40, kStrategy, kClose, kOpen, "BTC", OrderSide::Buy, 1.0);
    for (const double invalid : {std::numeric_limits<double>::quiet_NaN(),
             std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
        EXPECT_FALSE(guard.allows(buy, invalid, 2.0, 1.0));
        EXPECT_FALSE(guard.allows(buy, 5.0, invalid, 1.0));
        EXPECT_FALSE(guard.allows(buy, 5.0, 2.0, invalid));
    }
}

TEST(QuantityBuilder, R14_ExplicitQuantityTolerancePreventsMicroscopicOrders)
{
    OrderID nextId = 40;
    const auto result = QuantityOrderBuilder(0.001).createMarketPlan({quantityTarget(2.0005)},
        {held(2.0)}, OrderManager{}, kClose, kOpen, nextId);
    EXPECT_TRUE(result.empty());
    EXPECT_EQ(nextId, 40U);
    const ExecutionOrder buy(40, kStrategy, kClose, kOpen, "BTC", OrderSide::Buy, 0.0005);
    EXPECT_FALSE(OrderTargetGuard(0.001).allows(buy, 2.0005, 2.0, 0.0));
}

TEST(PlanningTolerance, R14_InvalidToleranceCannotDisableSafetyChecks)
{
    for (const double invalid : {-1.0, std::numeric_limits<double>::quiet_NaN(),
             std::numeric_limits<double>::infinity()}) {
        EXPECT_THROW({ const OrderTargetGuard guard(invalid); (void)guard; }, std::invalid_argument);
        EXPECT_THROW({ const QuantityOrderBuilder builder(invalid); (void)builder; }, std::invalid_argument);
        EXPECT_THROW({ const NotionalOrderPlanner planner(invalid); (void)planner; }, std::invalid_argument);
    }
}

} // namespace
