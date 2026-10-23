// LIVE long position stops (tests/specs/execution_state_service.md, Part B).

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

#include "position_stops.h"

namespace {

constexpr double FRACTION = 0.30;
constexpr Timestamp AT = 20261015;
constexpr OrderID NEXT_ID = 100;
const Coin BTC = "BTCUSDT";
const Coin ETH = "ETHUSDT";

StrategyPositionSnapshot holding(StrategyID strategy, const Coin& coin, double quantity)
{
    StrategyPositionSnapshot snapshot;
    snapshot[strategy].set(coin, quantity);
    return snapshot;
}

TrackedOrder stopOrder(OrderID id, StrategyID strategy, const Coin& coin, double quantity, double stopPrice,
                       ExecutionOrderStatus status, bool cancelRequested = false)
{
    ExecutionOrder request(id, strategy, 20261014, 20261014, coin, OrderSide::Sell, quantity);
    request.position_stop_price = stopPrice;
    request.reference_price = stopPrice / (1.0 - FRACTION);
    TrackedOrder order(request);
    order.status = status;
    order.cancel_requested = cancelRequested;
    return order;
}

void add(std::unordered_map<OrderID, TrackedOrder>& orders, const TrackedOrder& order)
{
    orders.emplace(order.request.order_id, order);
}

} // namespace


TEST(PositionStops, B1_HeldLongGetsOneWholePositionStopBelowTheClose)
{
    const auto changes = planPositionStops(holding(1, BTC, 0.5), {}, {{BTC, 60000.0}}, FRACTION, AT, NEXT_ID);
    ASSERT_EQ(changes.submits.size(), 1u);
    EXPECT_TRUE(changes.cancels.empty());
    const ExecutionOrder& stop = changes.submits.front();
    EXPECT_EQ(stop.order_id, NEXT_ID);
    EXPECT_EQ(stop.strategy_id, 1u);
    EXPECT_EQ(stop.coin, BTC);
    EXPECT_EQ(stop.side, OrderSide::Sell);
    EXPECT_DOUBLE_EQ(stop.quantity, 0.5);
    EXPECT_NEAR(stop.position_stop_price, 42000.0, 1e-6);
    EXPECT_NEAR(stop.reference_price, 60000.0, 1e-9);
    EXPECT_EQ(stop.created_at, AT);
    EXPECT_EQ(stop.active_from, AT);
    EXPECT_TRUE(stop.isPositionStop());
    EXPECT_FALSE(stop.fill_at_reference_price);
    EXPECT_NO_THROW(stop.validateConditional());
    EXPECT_EQ(changes.next_order_id, NEXT_ID + 1);
}

TEST(PositionStops, B2_AcceptedMatchingStopNeedsNoChange)
{
    std::unordered_map<OrderID, TrackedOrder> orders;
    add(orders, stopOrder(10, 1, BTC, 0.5, 42000.0, ExecutionOrderStatus::Accepted));
    const auto changes = planPositionStops(holding(1, BTC, 0.5), orders, {{BTC, 60000.0}}, FRACTION, AT, NEXT_ID);
    EXPECT_TRUE(changes.submits.empty());
    EXPECT_TRUE(changes.cancels.empty());
    EXPECT_EQ(changes.next_order_id, NEXT_ID);
}

TEST(PositionStops, B2_SubmittedMatchingStopIsNotDuplicated)
{
    std::unordered_map<OrderID, TrackedOrder> orders;
    add(orders, stopOrder(10, 1, BTC, 0.5, 42000.0, ExecutionOrderStatus::Submitted));
    const auto changes = planPositionStops(holding(1, BTC, 0.5), orders, {{BTC, 60000.0}}, FRACTION, AT, NEXT_ID);
    EXPECT_TRUE(changes.submits.empty());
    EXPECT_TRUE(changes.cancels.empty());
}

TEST(PositionStops, B3_StopMovesDownWithALowerClose)
{
    std::unordered_map<OrderID, TrackedOrder> orders;
    add(orders, stopOrder(10, 1, BTC, 0.5, 42000.0, ExecutionOrderStatus::Accepted));
    const auto changes = planPositionStops(holding(1, BTC, 0.5), orders, {{BTC, 50000.0}}, FRACTION, AT, NEXT_ID);
    ASSERT_EQ(changes.submits.size(), 1u);
    EXPECT_NEAR(changes.submits[0].position_stop_price, 35000.0, 1e-6);
    EXPECT_DOUBLE_EQ(changes.submits[0].quantity, 0.5);
    EXPECT_TRUE(changes.cancels.empty()) << "the old stop stays until the new one is accepted";
}

TEST(PositionStops, B3_StopMovesUpWithAHigherClose)
{
    std::unordered_map<OrderID, TrackedOrder> orders;
    add(orders, stopOrder(10, 1, BTC, 0.5, 42000.0, ExecutionOrderStatus::Accepted));
    const auto changes = planPositionStops(holding(1, BTC, 0.5), orders, {{BTC, 70000.0}}, FRACTION, AT, NEXT_ID);
    ASSERT_EQ(changes.submits.size(), 1u);
    EXPECT_NEAR(changes.submits[0].position_stop_price, 49000.0, 1e-6);
    EXPECT_TRUE(changes.cancels.empty());
}

TEST(PositionStops, B4_OldStopKeptWhileReplacementIsOnlySubmitted)
{
    std::unordered_map<OrderID, TrackedOrder> orders;
    add(orders, stopOrder(10, 1, BTC, 0.5, 42000.0, ExecutionOrderStatus::Accepted));
    add(orders, stopOrder(11, 1, BTC, 0.5, 35000.0, ExecutionOrderStatus::Submitted));
    const auto changes = planPositionStops(holding(1, BTC, 0.5), orders, {{BTC, 50000.0}}, FRACTION, AT, NEXT_ID);
    EXPECT_TRUE(changes.submits.empty());
    EXPECT_TRUE(changes.cancels.empty());
}

TEST(PositionStops, B4_OldStopCancelledOnceReplacementIsAccepted)
{
    for (const ExecutionOrderStatus status : {ExecutionOrderStatus::Accepted, ExecutionOrderStatus::PartiallyFilled}) {
        std::unordered_map<OrderID, TrackedOrder> orders;
        add(orders, stopOrder(10, 1, BTC, 0.5, 42000.0, ExecutionOrderStatus::Accepted));
        add(orders, stopOrder(11, 1, BTC, 0.5, 35000.0, status));
        const auto changes = planPositionStops(holding(1, BTC, 0.5), orders, {{BTC, 50000.0}}, FRACTION, AT, NEXT_ID);
        EXPECT_TRUE(changes.submits.empty());
        EXPECT_EQ(changes.cancels, (std::vector<OrderID>{10}));
    }
}

TEST(PositionStops, B5_StopIsResizedWhenThePositionGrows)
{
    std::unordered_map<OrderID, TrackedOrder> orders;
    add(orders, stopOrder(10, 1, BTC, 0.5, 42000.0, ExecutionOrderStatus::Accepted));
    const auto changes = planPositionStops(holding(1, BTC, 0.8), orders, {{BTC, 60000.0}}, FRACTION, AT, NEXT_ID);
    ASSERT_EQ(changes.submits.size(), 1u);
    EXPECT_DOUBLE_EQ(changes.submits[0].quantity, 0.8);
    EXPECT_NEAR(changes.submits[0].position_stop_price, 42000.0, 1e-6);
    EXPECT_TRUE(changes.cancels.empty());
}

TEST(PositionStops, B6_FlatPositionHasItsWorkingStopsCancelled)
{
    std::unordered_map<OrderID, TrackedOrder> orders;
    add(orders, stopOrder(10, 1, BTC, 0.5, 42000.0, ExecutionOrderStatus::Accepted));
    add(orders, stopOrder(11, 1, BTC, 0.5, 35000.0, ExecutionOrderStatus::Submitted));
    const auto changes = planPositionStops({}, orders, {{BTC, 60000.0}}, FRACTION, AT, NEXT_ID);
    EXPECT_TRUE(changes.submits.empty());
    std::vector<OrderID> cancels = changes.cancels;
    std::sort(cancels.begin(), cancels.end());
    EXPECT_EQ(cancels, (std::vector<OrderID>{10, 11}));
}

TEST(PositionStops, B6_FlatPositionLeavesTerminalAndCancellingStopsAlone)
{
    std::unordered_map<OrderID, TrackedOrder> orders;
    add(orders, stopOrder(10, 1, BTC, 0.5, 42000.0, ExecutionOrderStatus::Filled));
    add(orders, stopOrder(11, 1, BTC, 0.5, 42000.0, ExecutionOrderStatus::Canceled));
    add(orders, stopOrder(12, 1, BTC, 0.5, 42000.0, ExecutionOrderStatus::Rejected));
    add(orders, stopOrder(13, 1, BTC, 0.5, 42000.0, ExecutionOrderStatus::Accepted, true));
    StrategyPositionSnapshot positions;
    positions[1].set(ETH, 0.0); // explicitly zero
    const auto changes = planPositionStops(positions, orders, {{BTC, 60000.0}}, FRACTION, AT, NEXT_ID);
    EXPECT_TRUE(changes.submits.empty());
    EXPECT_TRUE(changes.cancels.empty());
}

TEST(PositionStops, B7_ShortsGetNoStop)
{
    const auto changes = planPositionStops(holding(1, BTC, -0.5), {}, {{BTC, 60000.0}}, FRACTION, AT, NEXT_ID);
    EXPECT_TRUE(changes.submits.empty());
    EXPECT_TRUE(changes.cancels.empty());
}

TEST(PositionStops, B8_MissingCloseIsReportedAndChangesNothing)
{
    std::unordered_map<OrderID, TrackedOrder> orders;
    add(orders, stopOrder(10, 1, BTC, 0.4, 42000.0, ExecutionOrderStatus::Accepted));
    for (const auto& closes : {std::unordered_map<Coin, double>{},
                               std::unordered_map<Coin, double>{{BTC, 0.0}},
                               std::unordered_map<Coin, double>{{BTC, -1.0}},
                               std::unordered_map<Coin, double>{{BTC, std::numeric_limits<double>::quiet_NaN()}}}) {
        const auto changes = planPositionStops(holding(1, BTC, 0.5), orders, closes, FRACTION, AT, NEXT_ID);
        EXPECT_TRUE(changes.submits.empty());
        EXPECT_TRUE(changes.cancels.empty());
        EXPECT_EQ(changes.missing_closes, (std::vector<std::string>{BTC}));
    }
}

TEST(PositionStops, B9_RejectedStopIsNotResubmittedForTheSameTerms)
{
    std::unordered_map<OrderID, TrackedOrder> orders;
    add(orders, stopOrder(12, 1, BTC, 0.5, 42000.0, ExecutionOrderStatus::Rejected));
    const auto changes = planPositionStops(holding(1, BTC, 0.5), orders, {{BTC, 60000.0}}, FRACTION, AT, NEXT_ID);
    EXPECT_TRUE(changes.submits.empty());
    EXPECT_EQ(changes.rejected_stops, (std::vector<OrderID>{12}));
}

TEST(PositionStops, B9_RejectedStopIsReplacedWhenThePriceChanges)
{
    std::unordered_map<OrderID, TrackedOrder> orders;
    add(orders, stopOrder(12, 1, BTC, 0.5, 42000.0, ExecutionOrderStatus::Rejected));
    const auto changes = planPositionStops(holding(1, BTC, 0.5), orders, {{BTC, 50000.0}}, FRACTION, AT, NEXT_ID);
    ASSERT_EQ(changes.submits.size(), 1u);
    EXPECT_NEAR(changes.submits[0].position_stop_price, 35000.0, 1e-6);
    EXPECT_TRUE(changes.rejected_stops.empty());
}

TEST(PositionStops, B10_EachStrategyGetsItsOwnStop)
{
    StrategyPositionSnapshot positions;
    positions[1].set(BTC, 0.5);
    positions[2].set(BTC, 0.2);
    const auto changes = planPositionStops(positions, {}, {{BTC, 60000.0}}, FRACTION, AT, NEXT_ID);
    ASSERT_EQ(changes.submits.size(), 2u);
    std::map<StrategyID, double> quantityByStrategy;
    for (const ExecutionOrder& stop : changes.submits) {
        quantityByStrategy[stop.strategy_id] = stop.quantity;
        EXPECT_NEAR(stop.position_stop_price, 42000.0, 1e-6);
        EXPECT_EQ(stop.side, OrderSide::Sell);
    }
    EXPECT_DOUBLE_EQ(quantityByStrategy.at(1), 0.5);
    EXPECT_DOUBLE_EQ(quantityByStrategy.at(2), 0.2);
}

TEST(PositionStops, B10_AnotherStrategysStopDoesNotCoverThisOne)
{
    StrategyPositionSnapshot positions;
    positions[1].set(BTC, 0.5);
    positions[2].set(BTC, 0.5);
    std::unordered_map<OrderID, TrackedOrder> orders;
    add(orders, stopOrder(10, 1, BTC, 0.5, 42000.0, ExecutionOrderStatus::Accepted));
    const auto changes = planPositionStops(positions, orders, {{BTC, 60000.0}}, FRACTION, AT, NEXT_ID);
    ASSERT_EQ(changes.submits.size(), 1u);
    EXPECT_EQ(changes.submits[0].strategy_id, 2u);
    EXPECT_TRUE(changes.cancels.empty());
}

TEST(PositionStops, B11_SameInputsSamePlanWithConsecutiveIds)
{
    StrategyPositionSnapshot positions;
    positions[1].set(BTC, 0.5);
    positions[1].set(ETH, 2.0);
    positions[2].set(BTC, 0.2);
    const std::unordered_map<Coin, double> closes = {{BTC, 60000.0}, {ETH, 2500.0}};
    const auto first = planPositionStops(positions, {}, closes, FRACTION, AT, NEXT_ID);
    const auto second = planPositionStops(positions, {}, closes, FRACTION, AT, NEXT_ID);
    ASSERT_EQ(first.submits.size(), 3u);
    ASSERT_EQ(second.submits.size(), 3u);
    std::set<OrderID> ids;
    for (std::size_t i = 0; i < first.submits.size(); ++i) {
        EXPECT_EQ(first.submits[i].order_id, second.submits[i].order_id);
        EXPECT_EQ(first.submits[i].strategy_id, second.submits[i].strategy_id);
        EXPECT_EQ(first.submits[i].coin, second.submits[i].coin);
        EXPECT_DOUBLE_EQ(first.submits[i].quantity, second.submits[i].quantity);
        EXPECT_DOUBLE_EQ(first.submits[i].position_stop_price, second.submits[i].position_stop_price);
        ids.insert(first.submits[i].order_id);
    }
    EXPECT_EQ(ids, (std::set<OrderID>{NEXT_ID, NEXT_ID + 1, NEXT_ID + 2}));
    EXPECT_EQ(first.next_order_id, NEXT_ID + 3);
}

TEST(PositionStops, B12_InvalidInputsAreRefused)
{
    const auto positions = holding(1, BTC, 0.5);
    const std::unordered_map<Coin, double> closes = {{BTC, 60000.0}};
    for (const double fraction : {0.0, 1.0, -0.1, 1.5, std::numeric_limits<double>::quiet_NaN(),
                                  std::numeric_limits<double>::infinity()})
        EXPECT_THROW(planPositionStops(positions, {}, closes, fraction, AT, NEXT_ID), std::invalid_argument)
            << "fraction " << fraction;
    EXPECT_THROW(planPositionStops(positions, {}, closes, FRACTION, 0, NEXT_ID), std::invalid_argument);
    EXPECT_THROW(planPositionStops(positions, {}, closes, FRACTION, AT, 0), std::invalid_argument);
}


TEST(PositionStops, B1_PositionBeingSoldEntirelyGetsNoNewStop)
{
    std::unordered_map<OrderID, TrackedOrder> orders;
    add(orders, stopOrder(10, 1, BTC, 0.5, 40000.0, ExecutionOrderStatus::Accepted)); // yesterday's stop
    ExecutionOrder exit(11, 1, 20261014, AT, BTC, OrderSide::Sell, 0.5);              // today's exit
    add(orders, TrackedOrder(exit));
    const auto changes = planPositionStops(holding(1, BTC, 0.5), orders, {{BTC, 60000.0}}, FRACTION, AT, NEXT_ID);
    EXPECT_TRUE(changes.submits.empty());
    EXPECT_TRUE(changes.cancels.empty()); // the old stop protects it until the sale fills

    // A partial sale (a trim) still gets the stop moved to the new close.
    orders.erase(11);
    ExecutionOrder trim(11, 1, 20261014, AT, BTC, OrderSide::Sell, 0.2);
    add(orders, TrackedOrder(trim));
    EXPECT_EQ(planPositionStops(holding(1, BTC, 0.5), orders, {{BTC, 60000.0}}, FRACTION, AT, NEXT_ID).submits.size(), 1u);
}
