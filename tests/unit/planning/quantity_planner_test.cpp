#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>
#include <vector>

#include "planning_test_support.h"

namespace {
using namespace planning_test;

TEST(QuantityPlanner, R10_FrozenCapitalResolvesAtNextOpenRatherThanDecisionClose)
{
    Inputs input;
    input.target("BTC", 250.0);
    input.prices.set("BTC", 125.0);
    const auto at125 = input.quantity();
    ASSERT_EQ(at125.execution_plan.orders_to_submit.size(), 1U);
    const auto& order = at125.execution_plan.orders_to_submit.front();
    EXPECT_EQ(order.side, OrderSide::Buy);
    EXPECT_DOUBLE_EQ(order.quantity, 2.0);
    EXPECT_DOUBLE_EQ(order.reference_price, 125.0);
    EXPECT_EQ(order.active_from, kOpen);
    EXPECT_DOUBLE_EQ(at125.global_target.get("BTC"), 250.0);

    input.prices.set("BTC", 50.0);
    const auto at50 = input.quantity();
    ASSERT_EQ(at50.execution_plan.orders_to_submit.size(), 1U);
    EXPECT_DOUBLE_EQ(at50.execution_plan.orders_to_submit.front().quantity, 5.0);
    EXPECT_DOUBLE_EQ(at50.global_target.get("BTC"), 250.0);
}

TEST(QuantityPlanner, R10_NegativeTargetOpensShortAndCanCrossLongThroughZero)
{
    Inputs input;
    input.target("BTC", -250.0, -0.25);
    input.prices.set("BTC", 125.0);
    const auto openShort = input.quantity();
    ASSERT_EQ(openShort.execution_plan.orders_to_submit.size(), 1U);
    EXPECT_EQ(openShort.execution_plan.orders_to_submit.front().side, OrderSide::Sell);
    EXPECT_DOUBLE_EQ(openShort.execution_plan.orders_to_submit.front().quantity, 2.0);

    input.positions[kStrategy].set("BTC", 2.0);
    const auto flip = input.quantity();
    ASSERT_EQ(flip.execution_plan.orders_to_submit.size(), 1U);
    EXPECT_EQ(flip.execution_plan.orders_to_submit.front().side, OrderSide::Sell);
    EXPECT_DOUBLE_EQ(flip.execution_plan.orders_to_submit.front().quantity, 4.0);
}

TEST(QuantityPlanner, R3_ExitFlattensActualLongAndShortDespitePriceGap)
{
    for (const double quantity : {3.0, -3.0}) {
        SCOPED_TRACE(quantity);
        Inputs input;
        input.positions[kStrategy].set("BTC", quantity);
        input.flat("BTC");
        input.prices.set("BTC", 250.0);
        const auto result = input.quantity();
        ASSERT_EQ(result.execution_plan.orders_to_submit.size(), 1U);
        const auto& order = result.execution_plan.orders_to_submit.front();
        EXPECT_EQ(order.side, quantity > 0.0 ? OrderSide::Sell : OrderSide::Buy);
        EXPECT_DOUBLE_EQ(order.quantity, 3.0);
        EXPECT_DOUBLE_EQ(result.global_target.get("BTC"), 0.0);
    }
}

TEST(QuantityPlanner, R4_MissingAssetAndExplicitHoldPreserveSignedFilledUnits)
{
    for (const double quantity : {3.0, -3.0}) {
        for (const bool explicitHold : {false, true}) {
            SCOPED_TRACE(quantity);
            SCOPED_TRACE(explicitHold);
            Inputs input;
            input.positions[kStrategy].set("BTC", quantity);
            input.prices.set("BTC", 125.0);
            input.flat("ETH");
            if (explicitHold)
                input.decisions.strategies.front().decisions["BTC"] = RebalanceDecision::hold();
            const auto result = input.quantity();
            EXPECT_TRUE(result.execution_plan.empty());
            EXPECT_DOUBLE_EQ(result.global_target.get("BTC"), quantity * 125.0);
            EXPECT_EQ(result.next_order_id, 40U);
        }
    }
}

TEST(QuantityPlanner, R4_MissingStrategyIntentPreservesItsFilledUnits)
{
    Inputs input;
    input.strategy_ids.push_back(kOtherStrategy);
    input.positions[kOtherStrategy].set("BTC", 3.0);
    input.prices.set("BTC", 125.0);
    input.flat("ETH");
    const auto result = input.quantity();
    EXPECT_TRUE(result.execution_plan.empty());
    EXPECT_DOUBLE_EQ(result.global_target.get("BTC"), 375.0);
}

TEST(QuantityPlanner, R4_AlreadyAtQuantityTargetDoesNotAllocateOrder)
{
    Inputs input;
    input.positions[kStrategy].set("BTC", 2.0);
    input.target("BTC", 250.0);
    input.prices.set("BTC", 125.0);
    const auto result = input.quantity();
    EXPECT_TRUE(result.execution_plan.empty());
    EXPECT_EQ(result.next_order_id, 40U);
}

TEST(QuantityPlanner, R5_RestoredPartialOrdersCoverLongAndShortTargets)
{
    for (const double sign : {1.0, -1.0}) {
        Inputs input;
        input.positions[kStrategy].set("BTC", sign * 2.0);
        input.target("BTC", sign * 500.0, sign * 0.5);
        input.orders.restore({pending(sign > 0.0 ? OrderSide::Buy : OrderSide::Sell)}, {101});
        const auto result = input.quantity();
        EXPECT_TRUE(result.execution_plan.empty());
        EXPECT_EQ(result.next_order_id, 40U);
    }
}

TEST(QuantityPlanner, R6_WaitsForCancelConfirmationBeforeSubmittingReplacement)
{
    Inputs input;
    input.positions[kStrategy].set("BTC", 2.0);
    input.target("BTC", 100.0, 0.1);
    input.orders.restore({pending()}, {101});
    const auto cancel = input.quantity();
    EXPECT_EQ(cancel.execution_plan.order_ids_to_cancel, std::vector<OrderID>{10});
    EXPECT_TRUE(cancel.execution_plan.orders_to_submit.empty());
    EXPECT_EQ(cancel.next_order_id, 40U);
    input.orders.markCancelRequested(10, kOpen);
    EXPECT_TRUE(input.quantity().execution_plan.empty());
    ASSERT_TRUE(input.orders.onOrderUpdate({10, kOpen, ExecutionOrderStatus::Canceled}));
    const auto replacement = input.quantity();
    ASSERT_EQ(replacement.execution_plan.orders_to_submit.size(), 1U);
    EXPECT_TRUE(replacement.execution_plan.order_ids_to_cancel.empty());
    EXPECT_EQ(replacement.execution_plan.orders_to_submit.front().side, OrderSide::Sell);
    EXPECT_DOUBLE_EQ(replacement.execution_plan.orders_to_submit.front().quantity, 1.0);
}

TEST(QuantityPlanner, R10_OpposingStrategyTargetsRetainSeparateAttribution)
{
    Inputs input;
    input.target("BTC", 250.0);
    input.strategy_ids.push_back(kOtherStrategy);
    auto other = input.decisions.strategies.front();
    other.strategy_id = kOtherStrategy;
    other.decisions["BTC"] = RebalanceDecision::targetWeight(-0.25);
    other.target_notional_usd["BTC"] = -250.0;
    input.decisions.strategies.push_back(other);
    input.prices.set("BTC", 125.0);
    const auto result = input.quantity();
    EXPECT_DOUBLE_EQ(result.global_target.get("BTC"), 0.0);
    ASSERT_EQ(result.execution_plan.orders_to_submit.size(), 2U);
    const auto& buy = result.execution_plan.orders_to_submit[0];
    const auto& sell = result.execution_plan.orders_to_submit[1];
    EXPECT_EQ(buy.strategy_id, kStrategy);
    EXPECT_EQ(buy.side, OrderSide::Buy);
    EXPECT_DOUBLE_EQ(buy.quantity, 2.0);
    EXPECT_EQ(sell.strategy_id, kOtherStrategy);
    EXPECT_EQ(sell.side, OrderSide::Sell);
    EXPECT_DOUBLE_EQ(sell.quantity, 2.0);
}

TEST(QuantityPlanner, R7_ReconstructedInputsAssignSameIdsByAsset)
{
    Inputs a;
    a.target("ETH", 100.0, 0.1);
    a.target("BTC", 250.0);
    Inputs b;
    b.target("BTC", 250.0);
    b.target("ETH", 100.0, 0.1);
    const auto first = a.quantity();
    const auto second = b.quantity();
    ASSERT_EQ(first.execution_plan.orders_to_submit.size(), 2U);
    ASSERT_EQ(second.execution_plan.orders_to_submit.size(), 2U);
    EXPECT_EQ(first.next_order_id, 42U);
    EXPECT_EQ(second.next_order_id, 42U);
    EXPECT_EQ(first.execution_plan.orders_to_submit[0].coin, "BTC");
    EXPECT_EQ(first.execution_plan.orders_to_submit[0].order_id, 40U);
    EXPECT_EQ(first.execution_plan.orders_to_submit[1].coin, "ETH");
    EXPECT_EQ(first.execution_plan.orders_to_submit[1].order_id, 41U);
    for (std::size_t i = 0; i < 2; ++i) {
        EXPECT_EQ(first.execution_plan.orders_to_submit[i].coin,
                  second.execution_plan.orders_to_submit[i].coin);
        EXPECT_EQ(first.execution_plan.orders_to_submit[i].order_id,
                  second.execution_plan.orders_to_submit[i].order_id);
        EXPECT_DOUBLE_EQ(first.execution_plan.orders_to_submit[i].quantity,
                         second.execution_plan.orders_to_submit[i].quantity);
    }
}

TEST(QuantityPlanner, R8_PlanningDoesNotAdvanceFilledOrPendingState)
{
    Inputs input;
    input.positions[kStrategy].set("BTC", 2.0);
    input.target("BTC", 500.0, 0.5);
    const auto before = makeExecutionPlanningStateSnapshot(input.strategy_ids,
        input.positions, input.orders, input.next_id);
    const auto first = input.quantity();
    const auto repeated = input.quantity();
    const auto after = makeExecutionPlanningStateSnapshot(input.strategy_ids,
        input.positions, input.orders, input.next_id);
    ASSERT_EQ(first.execution_plan.orders_to_submit.size(), 1U);
    ASSERT_EQ(repeated.execution_plan.orders_to_submit.size(), 1U);
    EXPECT_DOUBLE_EQ(first.execution_plan.orders_to_submit.front().quantity, 3.0);
    EXPECT_DOUBLE_EQ(repeated.execution_plan.orders_to_submit.front().quantity, 3.0);
    EXPECT_EQ(before.state_revision, after.state_revision);
    EXPECT_DOUBLE_EQ(input.positions.at(kStrategy).get("BTC"), 2.0);
    EXPECT_TRUE(input.orders.orders().empty());
}

TEST(QuantityPlanner, R11_RejectsExecutionAtOrBeforeDecisionClose)
{
    for (const Timestamp date : {kClose, Timestamp{20260104}}) {
        SCOPED_TRACE(date);
        Inputs input;
        input.target("BTC", 250.0);
        EXPECT_THROW(input.quantity(date), std::invalid_argument);
    }
}

TEST(QuantityPlanner, R11_RejectsAmbiguousStrategyIdentityAndDecisionDates)
{
    for (int invalid = 0; invalid < 6; ++invalid) {
        SCOPED_TRACE(invalid);
        Inputs input;
        input.target("BTC", 250.0);
        if (invalid == 0) input.next_id = 0;
        if (invalid == 1) input.strategy_ids.push_back(kStrategy);
        if (invalid == 2) input.decisions.strategies.push_back(input.decisions.strategies.front());
        if (invalid == 3) input.decisions.strategies.front().strategy_id = kOtherStrategy;
        if (invalid == 4) input.positions[kOtherStrategy].set("BTC", 2.0);
        if (invalid == 5) input.decisions.strategies.front().decision_timestamp = kOpen;
        EXPECT_THROW(input.quantity(), std::invalid_argument);
    }
}

TEST(QuantityPlanner, R11_EmptyDecisionBatchProducesNoCommands)
{
    Inputs input;
    input.decisions.strategies.clear();
    input.positions[kStrategy].set("BTC", 2.0);
    const auto result = input.quantity();
    EXPECT_TRUE(result.execution_plan.empty());
    EXPECT_EQ(result.next_order_id, 40U);
}

TEST(TargetResolution, R12_HoldFlatAndSignedWeightResolveWithoutChangingHoldings)
{
    Inputs input;
    input.positions[kStrategy].set("BTC", 3.0);
    input.positions[kStrategy].set("ETH", -2.0);
    input.prices.set("BTC", 125.0);
    input.flat("ETH");
    StrategyTargetResolver resolver;
    const auto heldTarget = resolver.resolve(input.decisions.strategies.front(),
        input.positions.at(kStrategy), input.prices);
    EXPECT_DOUBLE_EQ(heldTarget.get("BTC"), 375.0);
    EXPECT_DOUBLE_EQ(heldTarget.get("ETH"), 0.0);
    EXPECT_DOUBLE_EQ(input.positions.at(kStrategy).get("ETH"), -2.0);

    input.target("BTC", -250.0, -0.25);
    const auto shortTarget = resolver.resolve(input.decisions.strategies.front(),
        input.positions.at(kStrategy), input.prices);
    EXPECT_DOUBLE_EQ(shortTarget.get("BTC"), -250.0);
    const auto units = QuantityTargetResolver().resolve(shortTarget, input.prices);
    EXPECT_DOUBLE_EQ(units.get("BTC"), -2.0);
    EXPECT_DOUBLE_EQ(units.get("ETH"), 0.0);
}

TEST(TargetResolution, R12_CompatibilityPlanRetainsFrozenCapitalAndHoldSemantics)
{
    RebalancePlan plan(kClose, 1000.0);
    plan.set("BTC", RebalanceDecision::targetWeight(-0.25));
    VirtualPositionState positions;
    positions.set("ETH", -2.0);
    Inputs input;
    const auto result = StrategyTargetResolver().resolve(plan, positions, input.prices);
    EXPECT_DOUBLE_EQ(result.get("BTC"), -250.0);
    EXPECT_DOUBLE_EQ(result.get("ETH"), -100.0);
}

TEST(TargetResolution, R12_MissingHeldOrTargetPriceRejectsRatherThanFabricatesZero)
{
    Inputs input;
    input.positions[kStrategy].set("BTC", 3.0);
    input.flat("BTC");
    ExecutionReferencePrices missing;
    EXPECT_THROW(StrategyTargetResolver().resolve(input.decisions.strategies.front(),
        input.positions.at(kStrategy), missing), std::runtime_error);
    TargetPortfolio target;
    target.set("BTC", -250.0);
    EXPECT_THROW(QuantityTargetResolver().resolve(target, missing), std::runtime_error);
    input.prices = missing;
    EXPECT_THROW(input.quantity(), std::runtime_error);
}

TEST(QuantityPlanner, R10_NoImplicitVenueGridOrMinimumNotional)
{
    Inputs input;
    input.target("BTC", 1.0, 0.001);
    input.prices.set("BTC", 3.0);
    const auto result = input.quantity();
    ASSERT_EQ(result.execution_plan.orders_to_submit.size(), 1U);
    EXPECT_DOUBLE_EQ(result.execution_plan.orders_to_submit.front().quantity, 1.0 / 3.0);
    EXPECT_DOUBLE_EQ(result.global_target.get("BTC"), 1.0);
}

TEST(QuantityPlanner, R16_LongStopUsesTriggerAndActivatesAfterDecision)
{
    Inputs input;
    input.target("BTC", 250.0);
    input.decisions.strategies.front().decisions["BTC"] = RebalanceDecision::stopEntry(0.25, 100.0);
    input.prices.set("BTC", 90.0);
    const auto result = input.quantity();
    ASSERT_EQ(result.execution_plan.orders_to_submit.size(), 1U);
    const auto& order = result.execution_plan.orders_to_submit.front();
    EXPECT_EQ(order.side, OrderSide::Buy);
    EXPECT_DOUBLE_EQ(order.quantity, 2.5);
    EXPECT_DOUBLE_EQ(order.entry_stop_price, 100.0);
    EXPECT_EQ(order.created_at, kClose);
    EXPECT_EQ(order.active_from, kOpen);
}

TEST(QuantityPlanner, R16_ShortStopSizesAtLowerOfTriggerAndExecutionOpen)
{
    for (const double open : {80.0, 120.0}) {
        Inputs input;
        input.target("BTC", -250.0, -0.25);
        input.decisions.strategies.front().decisions["BTC"] = RebalanceDecision::stopEntry(-0.25, 100.0);
        input.prices.set("BTC", open);
        const auto result = input.quantity();
        ASSERT_EQ(result.execution_plan.orders_to_submit.size(), 1U);
        const auto& order = result.execution_plan.orders_to_submit.front();
        EXPECT_EQ(order.side, OrderSide::Sell);
        EXPECT_DOUBLE_EQ(order.quantity, open == 80.0 ? 3.125 : 2.5);
        EXPECT_DOUBLE_EQ(order.entry_stop_price, 100.0);
    }
}

TEST(QuantityPlanner, R16_RejectsHeldStopEntryAndNonfutureActivation)
{
    Inputs input;
    input.target("BTC", 250.0);
    input.decisions.strategies.front().decisions["BTC"] = RebalanceDecision::stopEntry(0.25, 100.0);
    input.positions[kStrategy].set("BTC", 1.0);
    EXPECT_THROW(input.quantity(), std::invalid_argument);
    input.positions.clear();
    EXPECT_THROW(input.quantity(kClose), std::invalid_argument);
}

TEST(QuantityPlanner, R17_ProtectiveCoverIsIgnoredForHoldAndCanceledForTimedExit)
{
    Inputs input;
    input.positions[kStrategy].set("BTC", -2.0);
    auto cover = pending(OrderSide::Buy, 2.0, 0.0);
    cover.request.parent_order_id = 8;
    cover.request.protective_stop_price = 120.0;
    input.orders.restore({cover}, {});
    const auto hold = input.quantity();
    EXPECT_TRUE(hold.execution_plan.empty());
    EXPECT_DOUBLE_EQ(hold.global_target.get("BTC"), -200.0);

    input.flat("BTC");
    const auto exit = input.quantity();
    EXPECT_EQ(exit.execution_plan.order_ids_to_cancel, std::vector<OrderID>{10});
    ASSERT_EQ(exit.execution_plan.orders_to_submit.size(), 1U);
    EXPECT_EQ(exit.execution_plan.orders_to_submit.front().side, OrderSide::Buy);
    EXPECT_DOUBLE_EQ(exit.execution_plan.orders_to_submit.front().quantity, 2.0);
    ASSERT_NE(input.orders.find(10), nullptr);
    EXPECT_FALSE(input.orders.find(10)->cancel_requested);
}

TEST(PlanningPrices, R9_NonpositiveOrNonfinitePricesCannotEnterPlannerInputs)
{
    for (const double bad : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
             std::numeric_limits<double>::infinity()}) {
        ExecutionReferencePrices prices;
        EXPECT_THROW(prices.set("BTC", bad), std::invalid_argument);
        EXPECT_FALSE(prices.contains("BTC"));
    }
}

} // namespace
