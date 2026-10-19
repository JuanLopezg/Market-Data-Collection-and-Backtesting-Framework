#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>
#include <vector>

#include "planning_test_support.h"

namespace {
using namespace planning_test;

TEST(NotionalPlanner, R1_ExplicitUsdTargetIsAuthoritativeAndCloseAuditable)
{
    Inputs input;
    input.target("BTC", 250.0, 0.9);
    const auto result = input.notional();
    ASSERT_EQ(result.submit_orders.size(), 1U);
    const auto& order = result.submit_orders.front();
    EXPECT_EQ(order.side, OrderSide::Buy);
    EXPECT_DOUBLE_EQ(order.notional_usd, 250.0);
    EXPECT_DOUBLE_EQ(order.target_notional_usd, 250.0);
    EXPECT_DOUBLE_EQ(order.reference_close, 100.0);
    EXPECT_EQ(order.created_at, kClose);
    EXPECT_EQ(order.decision_timestamp, kClose);
    EXPECT_EQ(order.state_revision, input.revision);
    EXPECT_EQ(order.strategy_id, kStrategy);
    EXPECT_DOUBLE_EQ(result.global_target.get("BTC"), 250.0);

    input.prices.set("BTC", 125.0);
    const auto changedClose = input.notional();
    ASSERT_EQ(changedClose.submit_orders.size(), 1U);
    EXPECT_DOUBLE_EQ(changedClose.submit_orders.front().notional_usd, 250.0);
}

TEST(NotionalPlanner, R2_BuyAddsOnlyMissingFilledExposure)
{
    Inputs input;
    input.positions[kStrategy].set("BTC", 2.0);
    input.target("BTC", 500.0, 0.5);
    const auto result = input.notional();
    ASSERT_EQ(result.submit_orders.size(), 1U);
    const auto& order = result.submit_orders.front();
    EXPECT_EQ(order.side, OrderSide::Buy);
    EXPECT_DOUBLE_EQ(order.current_notional_usd, 200.0);
    EXPECT_DOUBLE_EQ(order.pending_notional_usd, 0.0);
    EXPECT_DOUBLE_EQ(order.delta_notional_usd, 300.0);
    EXPECT_DOUBLE_EQ(order.notional_usd, 300.0);
}

TEST(NotionalPlanner, R2_ReduceLongIncreaseShortAndCrossZeroHaveCorrectSides)
{
    struct Example { double held; double target; double delta; };
    for (const auto& example : std::vector<Example>{{5.0, 200.0, -300.0},
             {-2.0, -500.0, -300.0}, {2.0, -300.0, -500.0}}) {
        SCOPED_TRACE(example.target);
        Inputs input;
        input.positions[kStrategy].set("BTC", example.held);
        input.target("BTC", example.target);
        const auto result = input.notional();
        ASSERT_EQ(result.submit_orders.size(), 1U);
        EXPECT_EQ(result.submit_orders.front().side, OrderSide::Sell);
        EXPECT_DOUBLE_EQ(result.submit_orders.front().delta_notional_usd, example.delta);
        EXPECT_DOUBLE_EQ(result.submit_orders.front().notional_usd, -example.delta);
    }
}

TEST(NotionalPlanner, R3_FlatUsesActualLongAndShortExposure)
{
    for (const double quantity : {3.0, -3.0}) {
        SCOPED_TRACE(quantity);
        Inputs input;
        input.positions[kStrategy].set("BTC", quantity);
        input.flat("BTC");
        const auto result = input.notional();
        ASSERT_EQ(result.submit_orders.size(), 1U);
        const auto& order = result.submit_orders.front();
        EXPECT_EQ(order.side, quantity > 0.0 ? OrderSide::Sell : OrderSide::Buy);
        EXPECT_DOUBLE_EQ(order.notional_usd, 300.0);
        EXPECT_DOUBLE_EQ(order.target_notional_usd, 0.0);
        EXPECT_DOUBLE_EQ(result.global_target.get("BTC"), 0.0);
    }
}

TEST(NotionalPlanner, R3_RepeatedFlatAtZeroAllocatesNoOrder)
{
    Inputs input;
    input.flat("BTC");
    const auto result = input.notional();
    EXPECT_TRUE(result.submit_orders.empty());
    EXPECT_TRUE(result.cancel_order_ids.empty());
    EXPECT_EQ(result.next_order_id, 40U);
}

TEST(NotionalPlanner, R4_MissingStrategyDecisionHoldsFilledUnits)
{
    Inputs input;
    input.decisions.strategies.clear();
    input.positions[kStrategy].set("BTC", 3.0);
    input.positions[kStrategy].set("ETH", -2.0);
    const auto result = input.notional();
    EXPECT_TRUE(result.submit_orders.empty());
    EXPECT_TRUE(result.cancel_order_ids.empty());
    EXPECT_DOUBLE_EQ(result.global_target.get("BTC"), 300.0);
    EXPECT_DOUBLE_EQ(result.global_target.get("ETH"), -100.0);
}

TEST(NotionalPlanner, R4_AlreadyAtUsdTargetDoesNotConsumeId)
{
    Inputs input;
    input.positions[kStrategy].set("BTC", 2.0);
    input.target("BTC", 200.0, 0.2);
    const auto result = input.notional();
    EXPECT_TRUE(result.submit_orders.empty());
    EXPECT_TRUE(result.cancel_order_ids.empty());
    EXPECT_EQ(result.next_order_id, 40U);
}

TEST(NotionalPlanner, R5_PartialLongAndShortRemaindersCoverTargetAfterRestore)
{
    for (const double sign : {1.0, -1.0}) {
        SCOPED_TRACE(sign);
        Inputs input;
        input.positions[kStrategy].set("BTC", sign * 2.0);
        input.target("BTC", sign * 500.0, sign * 0.5);
        input.orders.restore({pending(sign > 0.0 ? OrderSide::Buy : OrderSide::Sell)}, {101});
        const auto result = input.notional();
        EXPECT_TRUE(result.submit_orders.empty());
        EXPECT_TRUE(result.cancel_order_ids.empty());
        EXPECT_EQ(result.next_order_id, 40U);
    }
}

TEST(NotionalPlanner, R6_ConflictingPartialOrderCancelsBeforeReplacingAfterAck)
{
    Inputs input;
    input.positions[kStrategy].set("BTC", 2.0);
    input.target("BTC", 100.0, 0.1);
    input.orders.restore({pending()}, {101});
    const auto cancel = input.notional();
    EXPECT_EQ(cancel.cancel_order_ids, std::vector<OrderID>{10});
    EXPECT_TRUE(cancel.submit_orders.empty());
    EXPECT_EQ(cancel.next_order_id, 40U);

    input.orders.markCancelRequested(10, kOpen);
    const auto waiting = input.notional();
    EXPECT_TRUE(waiting.cancel_order_ids.empty());
    EXPECT_TRUE(waiting.submit_orders.empty());
    EXPECT_EQ(waiting.next_order_id, 40U);

    ASSERT_TRUE(input.orders.onOrderUpdate({10, kOpen, ExecutionOrderStatus::Canceled}));
    const auto replacement = input.notional();
    EXPECT_TRUE(replacement.cancel_order_ids.empty());
    ASSERT_EQ(replacement.submit_orders.size(), 1U);
    EXPECT_EQ(replacement.submit_orders.front().side, OrderSide::Sell);
    EXPECT_DOUBLE_EQ(replacement.submit_orders.front().notional_usd, 100.0);
    EXPECT_DOUBLE_EQ(replacement.submit_orders.front().pending_notional_usd, 0.0);
    EXPECT_EQ(replacement.next_order_id, 41U);
}

TEST(NotionalPlanner, R6_OrphanPendingEntryIsCanceledToHoldZero)
{
    Inputs input;
    input.orders.restore({pending(OrderSide::Buy, 2.0, 0.0)}, {});
    const auto result = input.notional();
    EXPECT_EQ(result.cancel_order_ids, std::vector<OrderID>{10});
    EXPECT_TRUE(result.submit_orders.empty());
}

TEST(NotionalPlanner, R7_RestartAndMapInsertionOrderPreserveEconomicIdentity)
{
    Inputs first;
    first.target("ETH", 100.0, 0.1);
    first.target("BTC", 250.0);
    Inputs rebuilt;
    rebuilt.target("BTC", 250.0);
    rebuilt.target("ETH", 100.0, 0.1);
    const auto a = first.notional();
    const auto b = rebuilt.notional();
    ASSERT_EQ(a.submit_orders.size(), 2U);
    ASSERT_EQ(b.submit_orders.size(), 2U);
    EXPECT_EQ(a.next_order_id, 42U);
    EXPECT_EQ(b.next_order_id, 42U);
    EXPECT_EQ(a.submit_orders[0].coin, "BTC");
    EXPECT_EQ(a.submit_orders[0].order_id, 40U);
    EXPECT_EQ(a.submit_orders[1].coin, "ETH");
    EXPECT_EQ(a.submit_orders[1].order_id, 41U);
    for (std::size_t i = 0; i < a.submit_orders.size(); ++i) {
        EXPECT_FALSE(a.submit_orders[i].economic_order_id.empty());
        EXPECT_EQ(a.submit_orders[i].economic_order_id, b.submit_orders[i].economic_order_id);
        EXPECT_EQ(a.submit_orders[i].coin, b.submit_orders[i].coin);
        EXPECT_DOUBLE_EQ(a.submit_orders[i].notional_usd, b.submit_orders[i].notional_usd);
    }
    rebuilt.revision = 124;
    const auto changedState = rebuilt.notional();
    ASSERT_EQ(changedState.submit_orders.size(), 2U);
    EXPECT_NE(a.submit_orders[0].economic_order_id, changedState.submit_orders[0].economic_order_id);
}

TEST(NotionalPlanner, R8_PlansDoNotApplyPositionsOrTrackOrders)
{
    Inputs input;
    input.positions[kStrategy].set("BTC", 2.0);
    input.target("BTC", 500.0, 0.5);
    const auto a = input.notional();
    const auto b = input.notional();
    EXPECT_DOUBLE_EQ(input.positions.at(kStrategy).get("BTC"), 2.0);
    EXPECT_TRUE(input.orders.orders().empty());
    EXPECT_EQ(input.next_id, 40U);
    ASSERT_EQ(a.submit_orders.size(), 1U);
    ASSERT_EQ(b.submit_orders.size(), 1U);
    EXPECT_DOUBLE_EQ(a.submit_orders.front().notional_usd, 300.0);
    EXPECT_EQ(a.submit_orders.front().economic_order_id, b.submit_orders.front().economic_order_id);
}

TEST(NotionalPlanner, R9_RejectsInvalidPlanningIdentityAndDates)
{
    for (int invalid = 0; invalid < 5; ++invalid) {
        SCOPED_TRACE(invalid);
        Inputs input;
        input.target("BTC", 250.0);
        if (invalid == 0) input.next_id = 0;
        if (invalid == 1) input.revision = 0;
        if (invalid == 2) input.decisions.decision_timestamp = kOpen;
        if (invalid == 3) input.decisions.strategies.front().decision_timestamp = kOpen;
        if (invalid == 4) {
            input.decisions.decision_timestamp = 0;
            input.decisions.strategies.front().decision_timestamp = 0;
            EXPECT_THROW(NotionalOrderPlanner().createPlan(input.strategy_ids, input.positions,
                input.orders, 0, input.revision, input.prices, input.decisions, input.next_id),
                std::invalid_argument);
            continue;
        }
        EXPECT_THROW(input.notional(), std::invalid_argument);
    }
}

TEST(NotionalPlanner, R9_RejectsAmbiguousStrategyAttribution)
{
    for (int invalid = 0; invalid < 4; ++invalid) {
        SCOPED_TRACE(invalid);
        Inputs input;
        input.target("BTC", 250.0);
        if (invalid == 0) input.strategy_ids.push_back(kStrategy);
        if (invalid == 1) input.decisions.strategies.push_back(input.decisions.strategies.front());
        if (invalid == 2) input.decisions.strategies.front().strategy_id = kOtherStrategy;
        if (invalid == 3) input.positions[kOtherStrategy].set("BTC", 2.0);
        EXPECT_THROW(input.notional(), std::invalid_argument);
    }
}

TEST(NotionalPlanner, R9_RejectsMissingOrOrphanUsdAndExplicitHold)
{
    for (int invalid = 0; invalid < 4; ++invalid) {
        SCOPED_TRACE(invalid);
        Inputs input;
        input.target("BTC", 250.0);
        auto& intent = input.decisions.strategies.front();
        if (invalid == 0) intent.target_notional_usd.clear();
        if (invalid == 1) intent.decisions.clear();
        if (invalid == 2) intent.decisions["BTC"] = RebalanceDecision::hold();
        if (invalid == 3) intent.decisions["BTC"] = RebalanceDecision::flat();
        EXPECT_THROW(input.notional(), std::invalid_argument);
    }
}

TEST(NotionalPlanner, R9_RejectsNonfiniteUsdAndMissingCloseForAnyEconomicState)
{
    for (const double bad : {std::numeric_limits<double>::quiet_NaN(),
             std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
        Inputs input;
        input.target("BTC", bad);
        EXPECT_THROW(input.notional(), std::invalid_argument);
    }
    for (int source = 0; source < 3; ++source) {
        SCOPED_TRACE(source);
        Inputs input;
        input.prices = ExecutionReferencePrices{};
        if (source == 0) input.target("BTC", 250.0);
        if (source == 1) input.positions[kStrategy].set("BTC", 2.0);
        if (source == 2) input.orders.restore({pending(OrderSide::Buy, 2.0, 0.0)}, {});
        EXPECT_THROW(input.notional(), std::runtime_error);
    }
}

TEST(NotionalPlanner, R14_ExplicitUsdToleranceSuppressesUneconomicResidue)
{
    Inputs input;
    input.positions[kStrategy].set("BTC", 2.0);
    input.target("BTC", 200.0005);
    const auto result = NotionalOrderPlanner(0.001).createPlan(input.strategy_ids,
        input.positions, input.orders, kClose, input.revision, input.prices,
        input.decisions, input.next_id);
    EXPECT_TRUE(result.submit_orders.empty());
    EXPECT_EQ(result.next_order_id, 40U);
}

TEST(NotionalPlanner, R1_NoImplicitVenueMinimumOrLotGrid)
{
    Inputs input;
    input.target("BTC", 1.0, 0.001);
    input.prices.set("BTC", 3.0);
    const auto result = input.notional();
    ASSERT_EQ(result.submit_orders.size(), 1U);
    EXPECT_DOUBLE_EQ(result.submit_orders.front().notional_usd, 1.0);
}

} // namespace
