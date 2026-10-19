#include <gtest/gtest.h>

#include <optional>
#include <stdexcept>
#include <vector>

#include "execution_test_support.h"

namespace {
using namespace execution_test;

ExecutionOrder shortBracket(OrderID id = 1, double quantity = 5.0)
{
    auto parent = order(id, OrderSide::Sell, quantity);
    parent.entry_stop_price = 100.0;
    parent.protective_stop_price = 120.0;
    return parent;
}

TEST(ExecutionEngine, R17_UnsupportedStopOrBracketIsRejectedWithoutStateOrCommands)
{
    for (int conditional = 0; conditional < 3; ++conditional) {
        Harness<> h;
        auto request = order(1);
        if (conditional == 0) request.entry_stop_price = 100.0;
        if (conditional == 1) request = shortBracket();
        if (conditional == 2) {
            request.parent_order_id = 8;
            request.protective_stop_price = 120.0;
        }
        EXPECT_THROW(h.engine.applyOrderPlan(plan({request}, 2), h.persist()), std::invalid_argument);
        EXPECT_TRUE(h.saved.empty());
        EXPECT_EQ(h.exchange.submitCount(), 0U);
        EXPECT_TRUE(h.engine.orderManager().orders().empty());
        EXPECT_EQ(h.engine.nextOrderId(), 1U);
        EXPECT_EQ(h.engine.lastExecutionTimestamp(), 0U);
    }
}

TEST(ExecutionEngine, R17_UnsupportedLaterCommandCannotLeakEarlierMarketSubmission)
{
    Harness<> h;
    auto conditional = order(2);
    conditional.entry_stop_price = 100.0;
    EXPECT_THROW(h.engine.applyOrderPlan(plan({order(1), conditional}, 3), h.persist()), std::invalid_argument);
    EXPECT_EQ(h.exchange.submitCount(), 0U);
    EXPECT_TRUE(h.engine.orderManager().orders().empty());
    EXPECT_TRUE(h.saved.empty());
    EXPECT_EQ(h.engine.nextOrderId(), 1U);
}

TEST(ExecutionEngine, R18_PartiallyFilledCanceledShortCreatesCoverForActualUnitsOnly)
{
    Harness<ObservedExchange> h;
    h.manual(shortBracket());
    h.exchange.venue.acceptOrder(1, kOpen);
    h.exchange.venue.fillOrder(1, 2.0, 100.0, kOpen);
    h.engine.processExchangeEvents(h.persist());
    EXPECT_EQ(h.exchange.venue.submitCount(), 1U);
    auto cancel = plan({}, 2);
    cancel.cancel_order_ids = {1};
    h.engine.applyOrderPlan(cancel, h.persist());
    h.exchange.venue.confirmCancel(1, kLater);
    h.engine.processExchangeEvents(h.persist());
    ASSERT_NE(h.engine.orderManager().find(2), nullptr);
    const auto& cover = h.engine.orderManager().find(2)->request;
    EXPECT_EQ(cover.parent_order_id, 1U);
    EXPECT_EQ(cover.strategy_id, kAlpha);
    EXPECT_EQ(cover.side, OrderSide::Buy);
    EXPECT_DOUBLE_EQ(cover.quantity, 2.0);
    EXPECT_DOUBLE_EQ(cover.protective_stop_price, 120.0);
    EXPECT_EQ(h.exchange.venue.submitCount(), 2U);
    EXPECT_DOUBLE_EQ(h.account.cash(), 1198.0);
    EXPECT_DOUBLE_EQ(h.engine.strategyPosition(kAlpha).get("BTC"), -2.0);
    EXPECT_DOUBLE_EQ(h.engine.orderManager().pendingSignedQuantity(kAlpha, "BTC"), 0.0);
}

TEST(ExecutionEngine, R18_RepeatedParentTerminalCannotRecreateFilledCover)
{
    Harness<ObservedExchange> h;
    h.manual(shortBracket(1, 2.0));
    h.exchange.venue.acceptOrder(1, kOpen);
    h.exchange.venue.fillOrder(1, 2.0, 100.0, kOpen);
    h.engine.processExchangeEvents(h.persist());
    ASSERT_NE(h.engine.orderManager().find(2), nullptr);
    h.exchange.venue.acceptOrder(2, kLater);
    h.exchange.venue.fillOrder(2, 2.0, 120.0, kLater);
    h.engine.processExchangeEvents(h.persist());
    EXPECT_DOUBLE_EQ(h.account.cash(), 955.60);
    EXPECT_DOUBLE_EQ(h.account.positions().get("BTC"), 0.0);
    EXPECT_DOUBLE_EQ(h.engine.strategyPosition(kAlpha).get("BTC"), 0.0);
    ASSERT_EQ(h.trades.closedTrades().size(), 1U);
    EXPECT_DOUBLE_EQ(h.trades.closedTrades().front().pnl, -44.40);
    ASSERT_TRUE(h.engine.processExchangeEvent(OrderUpdate{1, kLater, ExecutionOrderStatus::Filled}, h.persist()));
    EXPECT_EQ(h.exchange.venue.submitCount(), 2U);
    EXPECT_EQ(h.engine.nextOrderId(), 3U);
    EXPECT_EQ(h.engine.orderManager().orders().size(), 2U);
}

TEST(ExecutionEngine, R18_RestoredParentAndCanceledChildPreventDuplicateProtection)
{
    Harness<ObservedExchange> original;
    original.manual(shortBracket(1, 2.0));
    original.exchange.venue.acceptOrder(1, kOpen);
    original.exchange.venue.fillOrder(1, 2.0, 100.0, kOpen);
    original.engine.processExchangeEvents(original.persist());
    auto cancel = plan({}, 3);
    cancel.cancel_order_ids = {2};
    original.engine.applyOrderPlan(cancel, original.persist());
    original.exchange.venue.confirmCancel(2, kLater);
    original.engine.processExchangeEvents(original.persist());
    ASSERT_FALSE(original.saved.empty());
    Harness<ObservedExchange> restarted;
    restarted.restore(original.saved.back());
    ASSERT_TRUE(restarted.engine.processExchangeEvent(OrderUpdate{1, kLater, ExecutionOrderStatus::Filled}, restarted.persist()));
    EXPECT_TRUE(restarted.exchange.commands.empty());
    EXPECT_EQ(restarted.engine.nextOrderId(), 3U);
    EXPECT_EQ(restarted.engine.orderManager().orders().size(), 2U);
    EXPECT_DOUBLE_EQ(restarted.account.cash(), 1198.0);
    EXPECT_DOUBLE_EQ(restarted.engine.strategyPosition(kAlpha).get("BTC"), -2.0);
}

TEST(ExecutionEngine, R18_UnfilledCanceledOrRejectedParentCreatesNoCover)
{
    for (const auto terminal : {ExecutionOrderStatus::Canceled, ExecutionOrderStatus::Rejected}) {
        Harness<ObservedExchange> h;
        h.manual(shortBracket());
        ASSERT_TRUE(h.engine.processExchangeEvent(OrderUpdate{1, kOpen, terminal}, h.persist()));
        EXPECT_EQ(h.exchange.venue.submitCount(), 1U);
        EXPECT_EQ(h.engine.nextOrderId(), 2U);
        EXPECT_EQ(h.engine.orderManager().orders().size(), 1U);
        EXPECT_DOUBLE_EQ(h.account.cash(), 1000.0);
        EXPECT_DOUBLE_EQ(h.engine.strategyPosition(kAlpha).get("BTC"), 0.0);
    }
}

TEST(ExecutionEngine, R19_ReplacementPersistsBothIntentsAndSubmitsProtectionBeforeCancel)
{
    Harness<ObservedExchange> h;
    h.manual(order(1, OrderSide::Buy, 2.0));
    h.exchange.venue.acceptOrder(1, kOpen);
    h.exchange.venue.fillOrder(1, 2.0, 100.0, kOpen);
    h.engine.processExchangeEvents(h.persist());
    h.engine.applyOrderPlan(plan({}, 2), h.persist());
    auto oldStop = order(2, OrderSide::Sell, 2.0);
    oldStop.position_stop_price = 90.0;
    h.engine.applyPositionStopChanges({oldStop}, {}, 3, kOpen, h.persist());
    auto replacement = order(3, OrderSide::Sell, 2.0);
    replacement.position_stop_price = 95.0;
    bool firstCommit = true;
    h.engine.applyPositionStopChanges({replacement}, {2}, 4, kLater,
        [&](const std::optional<Fill>& incoming) {
            EXPECT_FALSE(incoming.has_value());
            if (firstCommit) {
                firstCommit = false;
                EXPECT_EQ(h.exchange.commands.size(), 2U);
                ASSERT_NE(h.engine.orderManager().find(3), nullptr);
                EXPECT_EQ(h.engine.orderManager().find(3)->status, ExecutionOrderStatus::Created);
                EXPECT_TRUE(h.engine.orderManager().find(2)->cancel_requested);
                EXPECT_EQ(h.engine.nextOrderId(), 4U);
            }
            EXPECT_EQ(h.engine.lastExecutionTimestamp(), kOpen);
            EXPECT_DOUBLE_EQ(h.account.cash(), 798.0);
            EXPECT_DOUBLE_EQ(h.account.positions().get("BTC"), 2.0);
            h.capture(incoming);
        });
    EXPECT_FALSE(firstCommit);
    ASSERT_EQ(h.exchange.commands.size(), 4U);
    EXPECT_TRUE(h.exchange.commands[2].submit);
    EXPECT_EQ(h.exchange.commands[2].id, 3U);
    EXPECT_FALSE(h.exchange.commands[3].submit);
    EXPECT_EQ(h.exchange.commands[3].id, 2U);
    EXPECT_DOUBLE_EQ(h.engine.orderManager().pendingSignedQuantity(kAlpha, "BTC"), 0.0);
    EXPECT_EQ(h.engine.lastExecutionTimestamp(), kOpen);
    EXPECT_DOUBLE_EQ(h.engine.lastGlobalTarget().get("BTC"), 0.0);
}

TEST(ExecutionEngine, R19_InvalidPositionStopChangesCannotStageOrDispatch)
{
    for (int invalid = 0; invalid < 6; ++invalid) {
        SCOPED_TRACE(invalid);
        Harness<ObservedExchange> h;
        auto old = order(1, OrderSide::Sell, 2.0);
        if (invalid != 2) old.position_stop_price = 90.0;
        h.manual(old);
        auto replacement = order(2, OrderSide::Sell, 2.0);
        replacement.position_stop_price = 95.0;
        std::vector<OrderID> cancels{1};
        if (invalid == 0) replacement.position_stop_price = 0.0;
        if (invalid == 1) cancels = {99};
        if (invalid == 3) cancels = {1, 1};
        if (invalid == 4) h.engine.orderManager().markCancelRequested(1, kOpen);
        if (invalid == 5) {
            ASSERT_TRUE(h.engine.processExchangeEvent(OrderUpdate{1, kOpen, ExecutionOrderStatus::Canceled}, h.persist()));
        }
        const auto committed = h.saved.size();
        EXPECT_THROW(h.engine.applyPositionStopChanges({replacement}, cancels, 3, kLater, h.persist()), std::invalid_argument);
        EXPECT_EQ(h.saved.size(), committed);
        EXPECT_EQ(h.exchange.commands.size(), 1U);
        EXPECT_EQ(h.engine.orderManager().orders().size(), 1U);
        EXPECT_EQ(h.engine.nextOrderId(), 2U);
        EXPECT_EQ(h.engine.lastExecutionTimestamp(), 0U);
    }
}

} // namespace
