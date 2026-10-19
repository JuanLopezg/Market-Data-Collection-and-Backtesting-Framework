#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>
#include <unordered_map>

#include "adapter_test_support.h"
#include "account.h"

namespace {
using namespace adapter_test;

ExecutionOrder cover()
{
    auto result = order(41);
    result.parent_order_id = 40;
    result.protective_stop_price = 120.0;
    result.created_at = kD2;
    result.active_from = kD2;
    return result;
}

ExecutionOrder positionStop(OrderID id = 40, double quantity = 2.0, double trigger = 90.0)
{
    auto result = order(id, OrderSide::Sell, quantity);
    result.position_stop_price = trigger;
    return result;
}

TEST(SimulatedExchange, R5_MarketOrderWaitsForEligibleOpenAndFillsOnlyOnce)
{
    SimulatedExchange exchange;
    exchange.submitOrder(order());
    const auto accepted = exchange.drainEvents();
    EXPECT_TRUE(eventsOf<Fill>(accepted).empty());
    ASSERT_EQ(eventsOf<OrderUpdate>(accepted).size(), 1U);
    EXPECT_EQ(eventsOf<OrderUpdate>(accepted).front().status, ExecutionOrderStatus::Accepted);
    exchange.processOpen(kD1, bars());
    EXPECT_TRUE(exchange.drainEvents().empty());
    exchange.processOpen(kD2, bars());
    const auto events = exchange.drainEvents();
    ASSERT_EQ(events.size(), 2U);
    ASSERT_TRUE(std::holds_alternative<Fill>(events[0]));
    ASSERT_TRUE(std::holds_alternative<OrderUpdate>(events[1]));
    EXPECT_DOUBLE_EQ(std::get<Fill>(events[0]).quantity, 2.0);
    EXPECT_DOUBLE_EQ(std::get<Fill>(events[0]).price, 125.0);
    EXPECT_EQ(std::get<Fill>(events[0]).timestamp, kD2);
    EXPECT_EQ(std::get<OrderUpdate>(events[1]).status, ExecutionOrderStatus::Filled);
    exchange.processOpen(kD3, bars());
    EXPECT_TRUE(exchange.drainEvents().empty());
}

TEST(SimulatedExchange, R5_OpenFillDoesNotUseHighLowOrCloseForItsPrice)
{
    for (const auto& observation : {bars(125.0, 140.0, 110.0, 130.0),
                                    bars(125.0, 10000.0, 1.0, 900.0)}) {
        SimulatedExchange exchange;
        exchange.submitOrder(order());
        exchange.drainEvents();
        exchange.processOpen(kD2, observation);
        const auto fills = eventsOf<Fill>(exchange.drainEvents());
        ASSERT_EQ(fills.size(), 1U);
        EXPECT_DOUBLE_EQ(fills.front().price, 125.0);
        EXPECT_DOUBLE_EQ(fills.front().quantity, 2.0);
    }
}

TEST(SimulatedExchange, R7_RealFakeAndSimulatorAgreeOnExplicitFillEconomicsAndFees)
{
    for (const auto side : {OrderSide::Buy, OrderSide::Sell}) {
        SimulatedExchange simulated(0.01);
        FakeExchange fake(1000.0, 0.01);
        const auto request = order(40, side);
        simulated.submitOrder(request);
        fake.submitOrder(request);
        fake.acceptOrder(40, kD2);
        const auto actual = fake.fillOrder(40, 2.0, 125.0, kD2);
        simulated.processOpen(kD2, bars());
        const auto fills = eventsOf<Fill>(simulated.drainEvents());
        ASSERT_EQ(fills.size(), 1U);
        EXPECT_EQ(fills.front().side, side);
        EXPECT_EQ(fills.front().timestamp, kD2);
        EXPECT_DOUBLE_EQ(fills.front().quantity, 2.0);
        EXPECT_DOUBLE_EQ(fills.front().price, 125.0);
        EXPECT_DOUBLE_EQ(fills.front().commission, 2.5);
        EXPECT_DOUBLE_EQ(fills.front().quantity, actual.quantity);
        EXPECT_DOUBLE_EQ(fills.front().commission, actual.commission);
        Account account(1000.0);
        account.applyFill(fills.front());
        EXPECT_DOUBLE_EQ(account.cash(), side == OrderSide::Buy ? 747.5 : 1247.5);
        PriceSnapshot marks;
        marks.set("BTC", 125.0);
        EXPECT_DOUBLE_EQ(account.equity(marks), 997.5);
    }
}

TEST(SimulatedExchange, R5_FillIdsFollowLocalOrderIdRatherThanInsertionOrder)
{
    SimulatedExchange exchange;
    exchange.submitOrder(order(41, OrderSide::Sell));
    exchange.submitOrder(order(40));
    exchange.processOpen(kD2, bars());
    const auto fills = eventsOf<Fill>(exchange.drainEvents());
    ASSERT_EQ(fills.size(), 2U);
    EXPECT_EQ(fills[0].order_id, 40U);
    EXPECT_EQ(fills[1].order_id, 41U);
    EXPECT_LT(fills[0].fill_id, fills[1].fill_id);
    EXPECT_GT(fills[0].fill_id, 0U);
}

TEST(SimulatedExchange, R6_CanceledOrderNeverFillsAndUnknownCancelCreatesNothing)
{
    SimulatedExchange exchange;
    exchange.submitOrder(order());
    exchange.drainEvents();
    exchange.cancelOrderAt(40, kD1);
    exchange.cancelOrder(99);
    const auto updates = eventsOf<OrderUpdate>(exchange.drainEvents());
    ASSERT_EQ(updates.size(), 1U);
    EXPECT_EQ(updates.front().order_id, 40U);
    EXPECT_EQ(updates.front().status, ExecutionOrderStatus::Canceled);
    exchange.processOpen(kD2, bars());
    EXPECT_TRUE(exchange.drainEvents().empty());
}

TEST(SimulatedExchange, R5_MissingMarketAssetDefersToItsNextAvailableOpen)
{
    SimulatedExchange exchange;
    exchange.submitOrder(order());
    exchange.drainEvents();
    exchange.processOpen(kD2, {});
    EXPECT_TRUE(exchange.drainEvents().empty());
    EXPECT_TRUE(exchange.activeOrders().contains(40));
    exchange.processOpen(kD3, bars());
    const auto fills = eventsOf<Fill>(exchange.drainEvents());
    ASSERT_EQ(fills.size(), 1U);
    EXPECT_EQ(fills.front().timestamp, kD3);
    EXPECT_DOUBLE_EQ(fills.front().price, 125.0);
}

TEST(SimulatedExchange, R6_InvalidOpenDoesNotConsumePendingMarketOrder)
{
    for (const double bad : {0.0, std::numeric_limits<double>::quiet_NaN()}) {
        SimulatedExchange exchange;
        exchange.submitOrder(order());
        exchange.drainEvents();
        auto invalid = bars();
        invalid.at("BTC").open = bad;
        EXPECT_THROW(exchange.processOpen(kD2, invalid), std::runtime_error);
        EXPECT_TRUE(exchange.activeOrders().contains(40));
        EXPECT_TRUE(eventsOf<Fill>(exchange.drainEvents()).empty());
        exchange.processOpen(kD2, bars());
        const auto fills = eventsOf<Fill>(exchange.drainEvents());
        ASSERT_EQ(fills.size(), 1U);
        EXPECT_DOUBLE_EQ(fills.front().quantity, 2.0);
    }
}

TEST(SimulatedExchange, R6_ManualExplicitPriceFillsOnlyChosenMarketOrder)
{
    SimulatedExchange exchange;
    exchange.submitOrder(order(40));
    exchange.submitOrder(order(41));
    exchange.drainEvents();
    exchange.fillAtPrice(40, kD2, 120.0);
    const auto fills = eventsOf<Fill>(exchange.drainEvents());
    ASSERT_EQ(fills.size(), 1U);
    EXPECT_EQ(fills.front().order_id, 40U);
    EXPECT_DOUBLE_EQ(fills.front().price, 120.0);
    EXPECT_TRUE(exchange.activeOrders().contains(41));
    auto conditional = order(42);
    conditional.entry_stop_price = 100.0;
    exchange.submitOrder(conditional);
    EXPECT_THROW(exchange.fillAtPrice(42, kD2, 120.0), std::invalid_argument);
    EXPECT_TRUE(exchange.activeOrders().contains(42));
}

TEST(SimulatedExchange, R7_SplitNextFillConservesUnitsFeesAndMoneyAndIsOneShot)
{
    SimulatedExchange exchange(0.01);
    exchange.configureSplitNextFill(0.25);
    exchange.submitOrder(order(40, OrderSide::Buy, 4.0));
    exchange.processOpen(kD2, bars(100.0, 110.0, 90.0, 100.0));
    const auto fills = eventsOf<Fill>(exchange.drainEvents());
    ASSERT_EQ(fills.size(), 2U);
    EXPECT_DOUBLE_EQ(fills[0].quantity, 1.0);
    EXPECT_DOUBLE_EQ(fills[1].quantity, 3.0);
    EXPECT_DOUBLE_EQ(fills[0].commission, 1.0);
    EXPECT_DOUBLE_EQ(fills[1].commission, 3.0);
    EXPECT_NE(fills[0].fill_id, fills[1].fill_id);
    Account account(1000.0);
    for (const auto& incoming : fills) account.applyFill(incoming);
    EXPECT_DOUBLE_EQ(account.cash(), 596.0);
    EXPECT_DOUBLE_EQ(account.positions().get("BTC"), 4.0);
    exchange.submitOrder(order(41));
    exchange.processOpen(kD3, bars());
    EXPECT_EQ(eventsOf<Fill>(exchange.drainEvents()).size(), 1U);
}

TEST(SimulatedExchange, R7_InjectedRejectAffectsOneOrderAndNeverCreatesFill)
{
    SimulatedExchange exchange;
    exchange.configureRejectNextOrder("synthetic rejection");
    exchange.submitOrder(order(40));
    const auto rejected = eventsOf<OrderUpdate>(exchange.drainEvents());
    ASSERT_EQ(rejected.size(), 1U);
    EXPECT_EQ(rejected.front().status, ExecutionOrderStatus::Rejected);
    EXPECT_EQ(rejected.front().message, "synthetic rejection");
    exchange.submitOrder(order(41));
    exchange.processOpen(kD2, bars());
    const auto fills = eventsOf<Fill>(exchange.drainEvents());
    ASSERT_EQ(fills.size(), 1U);
    EXPECT_EQ(fills.front().order_id, 41U);
}

TEST(SimulatedExchange, R6_InvalidFeeAndSplitConfigurationsReject)
{
    for (const double bad : {-0.01, std::numeric_limits<double>::quiet_NaN(),
             std::numeric_limits<double>::infinity()}) {
        EXPECT_THROW({ const SimulatedExchange invalid(bad); (void)invalid; }, std::invalid_argument);
    }
    SimulatedExchange exchange;
    for (const double bad : {0.0, 1.0, -0.1, std::numeric_limits<double>::quiet_NaN()})
        EXPECT_THROW(exchange.configureSplitNextFill(bad), std::invalid_argument);
}

TEST(SimulatedExchange, R8_RestoreEmitsNothingAndContinuesFillIdsFromSavedCounter)
{
    SimulatedExchange exchange;
    exchange.restoreState({{40, order(40)}, {41, order(41)}}, 101);
    EXPECT_TRUE(exchange.drainEvents().empty());
    exchange.processOpen(kD2, bars());
    const auto fills = eventsOf<Fill>(exchange.drainEvents());
    ASSERT_EQ(fills.size(), 2U);
    EXPECT_EQ(fills[0].fill_id, 101U);
    EXPECT_EQ(fills[1].fill_id, 102U);
    EXPECT_EQ(exchange.nextFillId(), 103U);
}

TEST(SimulatedExchange, R8_InvalidRestorePreservesExistingPendingOrderAndCounter)
{
    for (int invalid = 0; invalid < 3; ++invalid) {
        SimulatedExchange exchange;
        exchange.submitOrder(order());
        auto pending = std::unordered_map<OrderID, ExecutionOrder>{{41, order(41)}};
        FillID next = 101;
        if (invalid == 0) next = 0;
        if (invalid == 1) pending.at(41).order_id = 42;
        if (invalid == 2) pending.at(41).quantity = 0.0;
        EXPECT_THROW(exchange.restoreState(pending, next), std::invalid_argument);
        EXPECT_TRUE(exchange.activeOrders().contains(40));
        EXPECT_FALSE(exchange.activeOrders().contains(41));
        EXPECT_EQ(exchange.nextFillId(), 1U);
    }
}

TEST(SimulatedExchange, R9_LongStopWaitsForCompletedFollowingBarAndUsesTrigger)
{
    SimulatedExchange exchange;
    auto stop = order(40, OrderSide::Buy, 5.0);
    stop.entry_stop_price = 100.0;
    exchange.submitOrder(stop);
    exchange.drainEvents();
    const auto observation = bars(95.0, 105.0, 90.0, 100.0);
    exchange.processOpen(kD2, observation);
    EXPECT_TRUE(exchange.drainEvents().empty());
    exchange.processStopEntriesAtClose(kD1, observation);
    EXPECT_TRUE(exchange.drainEvents().empty());
    exchange.processStopEntriesAtClose(kD2, observation);
    const auto fills = eventsOf<Fill>(exchange.drainEvents());
    ASSERT_EQ(fills.size(), 1U);
    EXPECT_DOUBLE_EQ(fills.front().quantity, 5.0);
    EXPECT_DOUBLE_EQ(fills.front().price, 100.0);
    EXPECT_EQ(fills.front().timestamp, kD2);
}

TEST(SimulatedExchange, R9_LongGapPreservesNotionalAndCancelsUnusedEntryCap)
{
    SimulatedExchange exchange;
    auto stop = order(40, OrderSide::Buy, 5.0);
    stop.entry_stop_price = 100.0;
    exchange.submitOrder(stop);
    exchange.drainEvents();
    exchange.processStopEntriesAtClose(kD2, bars());
    const auto events = exchange.drainEvents();
    const auto fills = eventsOf<Fill>(events);
    const auto updates = eventsOf<OrderUpdate>(events);
    ASSERT_EQ(fills.size(), 1U);
    EXPECT_DOUBLE_EQ(fills.front().quantity, 4.0);
    EXPECT_DOUBLE_EQ(fills.front().price, 125.0);
    EXPECT_DOUBLE_EQ(fills.front().quantity * fills.front().price, 500.0);
    ASSERT_EQ(updates.size(), 1U);
    EXPECT_EQ(updates.front().status, ExecutionOrderStatus::Canceled);
    exchange.processStopEntriesAtClose(kD3, bars());
    EXPECT_TRUE(eventsOf<Fill>(exchange.drainEvents()).empty());
}

TEST(SimulatedExchange, R9_UntriggeredOrMissingAssetResearchEntryExpires)
{
    for (const bool missing : {false, true}) {
        SimulatedExchange exchange;
        auto stop = order(40, OrderSide::Buy, 5.0);
        stop.entry_stop_price = 100.0;
        exchange.submitOrder(stop);
        exchange.drainEvents();
        exchange.processStopEntriesAtClose(kD2, missing ? CoinBarMap{} : bars(95.0, 99.0, 90.0, 97.0));
        const auto events = exchange.drainEvents();
        EXPECT_TRUE(eventsOf<Fill>(events).empty());
        const auto updates = eventsOf<OrderUpdate>(events);
        ASSERT_EQ(updates.size(), 1U);
        EXPECT_EQ(updates.front().status, ExecutionOrderStatus::Canceled);
        exchange.processStopEntriesAtClose(kD3, bars());
        EXPECT_TRUE(exchange.drainEvents().empty());
    }
}

TEST(SimulatedExchange, R9_ShortGapUsesAlreadyPlannedQuantityWithoutSecondScaling)
{
    SimulatedExchange exchange;
    auto stop = order(40, OrderSide::Sell, 6.25);
    stop.entry_stop_price = 100.0;
    exchange.submitOrder(stop);
    exchange.processStopEntriesAtClose(kD2, bars(80.0, 90.0, 70.0, 75.0));
    const auto fills = eventsOf<Fill>(exchange.drainEvents());
    ASSERT_EQ(fills.size(), 1U);
    EXPECT_EQ(fills.front().side, OrderSide::Sell);
    EXPECT_DOUBLE_EQ(fills.front().quantity, 6.25);
    EXPECT_DOUBLE_EQ(fills.front().price, 80.0);
    EXPECT_DOUBLE_EQ(fills.front().quantity * fills.front().price, 500.0);
}

TEST(SimulatedExchange, R10_EntryBarProtectiveCoverRespectsBullishBearishOrdering)
{
    for (const bool bullish : {false, true}) {
        SimulatedExchange exchange;
        exchange.submitOrder(cover());
        exchange.drainEvents();
        exchange.processProtectiveStopsAtClose(kD2, bars(100.0, 125.0, 80.0, bullish ? 110.0 : 90.0));
        const auto fills = eventsOf<Fill>(exchange.drainEvents());
        if (bullish) {
            ASSERT_EQ(fills.size(), 1U);
            EXPECT_DOUBLE_EQ(fills.front().quantity, 2.0);
            EXPECT_DOUBLE_EQ(fills.front().price, 120.0);
        } else {
            EXPECT_TRUE(fills.empty());
            EXPECT_TRUE(exchange.activeOrders().contains(41));
        }
    }
}

TEST(SimulatedExchange, R10_ProtectiveCoverSurvivesMissingBarAndCoversLaterGap)
{
    SimulatedExchange exchange;
    exchange.submitOrder(cover());
    exchange.drainEvents();
    exchange.processProtectiveStopsAtClose(kD2, {});
    EXPECT_TRUE(exchange.drainEvents().empty());
    exchange.processProtectiveStopsAtClose(kD3, bars(130.0, 140.0, 120.0, 135.0));
    const auto fills = eventsOf<Fill>(exchange.drainEvents());
    ASSERT_EQ(fills.size(), 1U);
    EXPECT_EQ(fills.front().side, OrderSide::Buy);
    EXPECT_DOUBLE_EQ(fills.front().quantity, 2.0);
    EXPECT_DOUBLE_EQ(fills.front().price, 130.0);
}

TEST(SimulatedExchange, R10_CancelBeforeTimedExitPreventsDoubleCover)
{
    SimulatedExchange exchange;
    exchange.submitOrder(cover());
    exchange.cancelOrderAt(41, kD3);
    auto timed = order(42, OrderSide::Buy);
    timed.created_at = kD2;
    timed.active_from = kD3;
    exchange.submitOrder(timed);
    exchange.drainEvents();
    const auto observation = bars(130.0, 140.0, 120.0, 135.0);
    exchange.processOpen(kD3, observation);
    exchange.processProtectiveStopsAtClose(kD3, observation);
    const auto fills = eventsOf<Fill>(exchange.drainEvents());
    ASSERT_EQ(fills.size(), 1U);
    EXPECT_EQ(fills.front().order_id, 42U);
    EXPECT_DOUBLE_EQ(fills.front().quantity, 2.0);
}

TEST(SimulatedExchange, R11_PositionStopWaitsForCompletedBarAndHandlesDownwardGap)
{
    for (const double open : {100.0, 80.0}) {
        SimulatedExchange exchange;
        exchange.submitOrder(positionStop());
        exchange.drainEvents();
        const auto observation = bars(open, 110.0, 70.0, 90.0);
        exchange.processOpen(kD2, observation);
        EXPECT_TRUE(exchange.drainEvents().empty());
        PositionState positions;
        positions.set("BTC", 2.0);
        exchange.processPositionStopsAtClose(kD2, observation, positions);
        const auto fills = eventsOf<Fill>(exchange.drainEvents());
        ASSERT_EQ(fills.size(), 1U);
        EXPECT_EQ(fills.front().side, OrderSide::Sell);
        EXPECT_DOUBLE_EQ(fills.front().quantity, 2.0);
        EXPECT_DOUBLE_EQ(fills.front().price, open == 80.0 ? 80.0 : 90.0);
    }
}

TEST(SimulatedExchange, R11_OversizedPositionStopSellsOnlyActuallyHeldLong)
{
    SimulatedExchange exchange(0.01);
    exchange.submitOrder(positionStop(40, 5.0));
    PositionState positions;
    positions.set("BTC", 2.0);
    exchange.processPositionStopsAtClose(kD2, bars(80.0, 100.0, 70.0, 90.0), positions);
    const auto fills = eventsOf<Fill>(exchange.drainEvents());
    ASSERT_EQ(fills.size(), 1U);
    EXPECT_DOUBLE_EQ(fills.front().quantity, 2.0);
    EXPECT_DOUBLE_EQ(fills.front().commission, 1.6);
    EXPECT_DOUBLE_EQ(positions.get("BTC"), 2.0); // Caller applies emitted fills.
    exchange.processPositionStopsAtClose(kD3, bars(80.0, 100.0, 70.0, 90.0), positions);
    EXPECT_TRUE(eventsOf<Fill>(exchange.drainEvents()).empty());
}

TEST(SimulatedExchange, R11_OverlappingStopsReserveUnitsInHigherStopPriceOrder)
{
    SimulatedExchange exchange;
    exchange.submitOrder(positionStop(40, 2.0, 90.0));
    exchange.submitOrder(positionStop(41, 2.0, 95.0));
    PositionState positions;
    positions.set("BTC", 3.0);
    exchange.processPositionStopsAtClose(kD2, bars(100.0, 110.0, 80.0, 90.0), positions);
    const auto fills = eventsOf<Fill>(exchange.drainEvents());
    ASSERT_EQ(fills.size(), 2U);
    EXPECT_EQ(fills[0].order_id, 41U);
    EXPECT_DOUBLE_EQ(fills[0].quantity, 2.0);
    EXPECT_DOUBLE_EQ(fills[0].price, 95.0);
    EXPECT_EQ(fills[1].order_id, 40U);
    EXPECT_DOUBLE_EQ(fills[1].quantity, 1.0);
    EXPECT_DOUBLE_EQ(fills[1].price, 90.0);
    EXPECT_DOUBLE_EQ(fills[0].quantity + fills[1].quantity, 3.0);
}

TEST(SimulatedExchange, R11_PositionStopCannotOpenShortFromFlatOrIncreaseExistingShort)
{
    for (const double held : {0.0, -2.0}) {
        SimulatedExchange exchange;
        exchange.submitOrder(positionStop());
        exchange.drainEvents();
        PositionState positions;
        positions.set("BTC", held);
        exchange.processPositionStopsAtClose(kD2, bars(80.0, 100.0, 70.0, 90.0), positions);
        const auto events = exchange.drainEvents();
        EXPECT_TRUE(eventsOf<Fill>(events).empty());
        const auto updates = eventsOf<OrderUpdate>(events);
        ASSERT_EQ(updates.size(), 1U);
        EXPECT_EQ(updates.front().status, ExecutionOrderStatus::Canceled);
    }
}

TEST(SimulatedExchange, R11_MissingOrUntouchedBarDoesNotExpireRestingPositionStop)
{
    SimulatedExchange exchange;
    exchange.submitOrder(positionStop());
    exchange.drainEvents();
    PositionState positions;
    positions.set("BTC", 2.0);
    exchange.processPositionStopsAtClose(kD2, bars(100.0, 110.0, 95.0, 100.0), positions);
    exchange.processPositionStopsAtClose(kD3, {}, positions);
    EXPECT_TRUE(exchange.drainEvents().empty());
    EXPECT_TRUE(exchange.activeOrders().contains(40));
    exchange.processPositionStopsAtClose(20260108, bars(100.0, 110.0, 80.0, 90.0), positions);
    const auto fills = eventsOf<Fill>(exchange.drainEvents());
    ASSERT_EQ(fills.size(), 1U);
    EXPECT_DOUBLE_EQ(fills.front().quantity, 2.0);
}

TEST(SimulatedExchange, R6_DuplicateActiveIdCannotReplaceOriginalEconomicOrder)
{
    SimulatedExchange exchange;
    exchange.submitOrder(order());
    EXPECT_THROW(exchange.submitOrder(order(40, OrderSide::Sell, 5.0)), std::invalid_argument);
    exchange.processOpen(kD2, bars());
    const auto fills = eventsOf<Fill>(exchange.drainEvents());
    ASSERT_EQ(fills.size(), 1U);
    EXPECT_EQ(fills.front().side, OrderSide::Buy);
    EXPECT_DOUBLE_EQ(fills.front().quantity, 2.0);
}

} // namespace
