#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "mock_test_support.h"

namespace {
using namespace mock_test;

TEST(MockMatching, R5_NoFillWithoutAdmissionOrBeforeActivation)
{
    M::MockOrders life;
    M::MockMatching matcher(life, matching());
    matcher.processBar(bar());
    EXPECT_TRUE(eventsOf<V::Fill>(matcher.emittedEvents()).empty());
    life.submit(submit(order(2, V::Side::Buy, 0.01), "rejected"));
    matcher.processBar(bar(21));
    EXPECT_TRUE(eventsOf<V::Fill>(matcher.emittedEvents()).empty());
    life.submit(submit());
    matcher.clearOutput();
    matcher.processBar(bar(19));
    EXPECT_TRUE(eventsOf<V::Fill>(matcher.emittedEvents()).empty());
    matcher.processBar(bar(20));
    const auto fills = eventsOf<V::Fill>(matcher.emittedEvents());
    ASSERT_EQ(fills.size(), 1U);
    EXPECT_EQ(fills.front().local_order_id, 1U);
    EXPECT_EQ(fills.front().timestamp, 20U);
    EXPECT_DOUBLE_EQ(fills.front().quantity, 0.1);
}

TEST(MockMatching, R6_BuyAndSellIntrabarTouchFillAtLimitBeforeLifecycleUpdate)
{
    for (const auto side : {V::Side::Buy, V::Side::Sell}) {
        M::MockOrders life;
        life.submit(submit(order(1, side)));
        M::MockMatching matcher(life, matching());
        matcher.processBar(bar(20, 10.0, side == V::Side::Buy ? 101.0 : 99.0,
                               102.0, 98.0, 100.0));
        const auto fills = eventsOf<V::Fill>(matcher.emittedEvents());
        ASSERT_EQ(fills.size(), 1U);
        EXPECT_EQ(fills.front().side, side);
        EXPECT_DOUBLE_EQ(fills.front().price, 100.0);
        EXPECT_DOUBLE_EQ(fills.front().quantity, 0.2);
        ASSERT_GE(matcher.emittedEvents().size(), 2U);
        EXPECT_TRUE(std::holds_alternative<V::Fill>(matcher.emittedEvents()[0]));
        EXPECT_TRUE(std::holds_alternative<V::OrderUpdate>(matcher.emittedEvents()[1]));
        EXPECT_TRUE(eventsOf<V::AccountingEvent>(matcher.emittedEvents()).empty());
    }
}

TEST(MockMatching, R6_NoTouchOrZeroCapacityRestsGtcAndCancelsIocWithoutFills)
{
    for (const auto tif : {V::TimeInForce::Gtc, V::TimeInForce::Ioc}) {
        for (const bool zeroCapacity : {false, true}) {
            M::MockOrders life;
            auto intent = order(1, V::Side::Buy, 0.2, zeroCapacity ? 100.0 : 90.0);
            intent.time_in_force = tif;
            life.submit(submit(intent));
            M::MockMatching matcher(life, matching());
            matcher.processBar(bar(20, zeroCapacity ? 0.0 : 10.0));
            EXPECT_TRUE(eventsOf<V::Fill>(matcher.emittedEvents()).empty());
            EXPECT_EQ(life.findOrder(1)->status, tif == V::TimeInForce::Gtc
                ? V::OrderLifecycleStatus::Resting : V::OrderLifecycleStatus::Canceled);
            EXPECT_DOUBLE_EQ(life.findOrder(1)->cumulative_filled_quantity, 0.0);
        }
    }
}

TEST(MockMatching, R7_SharedCapacityGoesToLowestOrderIdRegardlessOfInsertionOrder)
{
    M::MockOrders life;
    auto batch = submit(order(2));
    batch.items.push_back({"earlier-order", order(1)});
    life.submit(batch);
    M::MockMatching matcher(life, matching());
    matcher.processBar(bar());
    const auto fills = eventsOf<V::Fill>(matcher.emittedEvents());
    ASSERT_EQ(fills.size(), 1U);
    EXPECT_EQ(fills.front().local_order_id, 1U);
    EXPECT_DOUBLE_EQ(fills.front().quantity, 0.1);
    EXPECT_DOUBLE_EQ(life.findOrder(1)->cumulative_filled_quantity, 0.1);
    EXPECT_DOUBLE_EQ(life.findOrder(2)->cumulative_filled_quantity, 0.0);
    EXPECT_EQ(life.findOrder(1)->status, V::OrderLifecycleStatus::PartiallyFilled);
}

TEST(MockMatching, R7_TwoPartialBarsCompleteExactlyRequestedQuantityWithoutLaterOverfill)
{
    M::MockOrders life;
    life.submit(submit());
    M::MockMatching matcher(life, matching());
    matcher.processBar(bar(20));
    matcher.processBar(bar(30));
    matcher.processBar(bar(40, 100.0));
    const auto fills = eventsOf<V::Fill>(matcher.emittedEvents());
    ASSERT_EQ(fills.size(), 2U);
    EXPECT_DOUBLE_EQ(fills[0].quantity, 0.1);
    EXPECT_DOUBLE_EQ(fills[1].quantity, 0.1);
    EXPECT_NE(fills[0].native_references.native_fill_id, fills[1].native_references.native_fill_id);
    EXPECT_DOUBLE_EQ(life.findOrder(1)->cumulative_filled_quantity, 0.2);
    EXPECT_DOUBLE_EQ(life.findOrder(1)->remaining_quantity, 0.0);
    EXPECT_EQ(life.findOrder(1)->status, V::OrderLifecycleStatus::Filled);
}

TEST(MockMatching, R7_PartialIocEmitsFillThenPartialThenCanceledAndNeverFillsRemainder)
{
    M::MockOrders life;
    auto intent = order();
    intent.time_in_force = V::TimeInForce::Ioc;
    life.submit(submit(intent));
    M::MockMatching matcher(life, matching());
    matcher.processBar(bar());
    const auto& events = matcher.emittedEvents();
    ASSERT_EQ(events.size(), 3U);
    ASSERT_TRUE(std::holds_alternative<V::Fill>(events[0]));
    EXPECT_DOUBLE_EQ(std::get<V::Fill>(events[0]).quantity, 0.1);
    ASSERT_TRUE(std::holds_alternative<V::OrderUpdate>(events[1]));
    ASSERT_TRUE(std::holds_alternative<V::OrderUpdate>(events[2]));
    EXPECT_EQ(std::get<V::OrderUpdate>(events[1]).status, V::OrderLifecycleStatus::PartiallyFilled);
    EXPECT_EQ(std::get<V::OrderUpdate>(events[2]).status, V::OrderLifecycleStatus::Canceled);
    matcher.clearOutput();
    matcher.processBar(bar(30, 100.0));
    EXPECT_TRUE(eventsOf<V::Fill>(matcher.emittedEvents()).empty());
    EXPECT_DOUBLE_EQ(life.findOrder(1)->cumulative_filled_quantity, 0.1);
}

TEST(MockMatching, R6_PostOnlyCrossingAtArrivalRejectsBothSidesWithoutFills)
{
    for (const auto side : {V::Side::Buy, V::Side::Sell}) {
        M::MockOrders life;
        auto intent = order(1, side);
        intent.post_only = true;
        life.submit(submit(intent));
        M::MockMatching matcher(life, matching());
        matcher.processBar(bar());
        EXPECT_TRUE(eventsOf<V::Fill>(matcher.emittedEvents()).empty());
        const auto updates = eventsOf<V::OrderUpdate>(matcher.emittedEvents());
        ASSERT_EQ(updates.size(), 1U);
        EXPECT_EQ(updates.front().status, V::OrderLifecycleStatus::Rejected);
        EXPECT_EQ(updates.front().terminal_reason.classification, V::ErrorClass::PostOnlyWouldCross);
    }
}

TEST(MockMatching, R8_LatencyCountsOnlyEligibleSameAssetObservations)
{
    M::MockOrders life;
    life.submit(submit());
    auto config = matching();
    config.latency_events = 1;
    M::MockMatching matcher(life, config);
    matcher.processBar(bar(19));
    matcher.processBar(bar(20));
    matcher.processBar(bar(21, 1.0, 100.0, 101.0, 99.0, 100.0, "ETHUSDT"));
    EXPECT_TRUE(eventsOf<V::Fill>(matcher.emittedEvents()).empty());
    matcher.processBar(bar(22));
    const auto fills = eventsOf<V::Fill>(matcher.emittedEvents());
    ASSERT_EQ(fills.size(), 1U);
    EXPECT_EQ(fills.front().timestamp, 22U);
    EXPECT_DOUBLE_EQ(fills.front().quantity, 0.1);
}

TEST(MockMatching, R8_SlippageIsBoundedSideCorrectOnGridAndReproducible)
{
    for (const auto side : {V::Side::Buy, V::Side::Sell}) {
        std::vector<V::Fill> first;
        for (int replay = 0; replay < 2; ++replay) {
            M::MockOrders life;
            life.submit(submit(order(1, side)));
            M::MatchingFillConfig config;
            M::MockMatching matcher(life, config);
            matcher.processBar(bar(20, 10.0, side == V::Side::Buy ? 99.0 : 101.0,
                                   102.0, 98.0, 100.0));
            const auto fills = eventsOf<V::Fill>(matcher.emittedEvents());
            ASSERT_EQ(fills.size(), 1U);
            const auto& execution = fills.front();
            if (side == V::Side::Buy) {
                EXPECT_GE(execution.price, 99.0);
                EXPECT_LE(execution.price, 99.02475 + 1e-8);
                EXPECT_LE(execution.price, 100.0);
            } else {
                EXPECT_GE(execution.price, 100.97475 - 1e-8);
                EXPECT_LE(execution.price, 101.0);
                EXPECT_GE(execution.price, 100.0);
            }
            EXPECT_TRUE(M::divisibleByIncrement(execution.price, "0.00000001", 8));
            if (replay == 0) first = fills;
            else {
                EXPECT_DOUBLE_EQ(execution.price, first.front().price);
                EXPECT_DOUBLE_EQ(execution.quantity, first.front().quantity);
                EXPECT_EQ(execution.native_references.native_fill_id, first.front().native_references.native_fill_id);
            }
        }
    }
}

TEST(MockVenueSafety, DISABLED_R9_ReduceOnlyOrderFromFlatCannotCreateExposure)
{
    // FINDING: F2 - Reduce-only BUY 0.2 from flat fills and opens +0.2, charging
    // a fee; R9 requires the flag to prevent creating exposure.
    test_support::TempDir directory("mock-reduce-flat");
    M::MockRecovery runtime(directory.path(), matching());
    auto intent = order();
    intent.reduce_only = true;
    EXPECT_EQ(runtime.submit(submit(intent)), M::RecoverySourceResult::Applied);
    EXPECT_EQ(runtime.processBar(bar(20, 10.0)), M::RecoverySourceResult::Applied);
    const auto snapshot = runtime.stateSnapshot(20);
    EXPECT_TRUE(snapshot.fills.fills.empty());
    for (const auto& position : snapshot.account.positions)
        EXPECT_DOUBLE_EQ(position.signed_quantity, 0.0);
    EXPECT_EQ(runtime.account().settledCashUnits(), 10000000000000LL);
}

TEST(MockVenueSafety, DISABLED_R9_OversizedReduceOnlyExitCannotFlipLongToShort)
{
    // FINDING: F2 - Reduce-only SELL 0.3 against held +0.2 fills all 0.3 and
    // leaves -0.1; R9 permits rejection/clamping but never crossing zero.
    test_support::TempDir directory("mock-reduce-exit");
    M::MockRecovery runtime(directory.path(), matching());
    ASSERT_EQ(runtime.submit(submit()), M::RecoverySourceResult::Applied);
    ASSERT_EQ(runtime.processBar(bar(20, 10.0)), M::RecoverySourceResult::Applied);
    auto exit = order(2, V::Side::Sell, 0.3);
    exit.reduce_only = true;
    ASSERT_EQ(runtime.submit(submit(exit, "exit", 30)), M::RecoverySourceResult::Applied);
    ASSERT_EQ(runtime.processBar(bar(40, 10.0)), M::RecoverySourceResult::Applied);
    const auto snapshot = runtime.stateSnapshot(40);
    for (const auto& position : snapshot.account.positions) {
        EXPECT_GE(position.signed_quantity, 0.0);
        EXPECT_LE(position.signed_quantity, 0.2);
    }
    double exitQuantity = 0.0;
    for (const auto& execution : snapshot.fills.fills)
        if (execution.local_order_id == 2) exitQuantity += execution.quantity;
    EXPECT_LE(exitQuantity, 0.2);
}

} // namespace
