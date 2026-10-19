#include <gtest/gtest.h>

#include <limits>
#include <optional>
#include <stdexcept>

#include "execution_test_support.h"

namespace {
using namespace execution_test;

TEST(ExecutionEngine, R1_SubmissionAcceptanceAndStatusOnlyFilledDoNotMoveMoney)
{
    Harness<> h;
    h.manual(order());
    EXPECT_DOUBLE_EQ(h.account.cash(), 1000.0);
    EXPECT_DOUBLE_EQ(h.account.positions().get("BTC"), 0.0);
    EXPECT_DOUBLE_EQ(h.engine.strategyPosition(kAlpha).get("BTC"), 0.0);
    h.exchange.acceptOrder(10, kOpen);
    h.engine.processExchangeEvents(h.persist());
    EXPECT_DOUBLE_EQ(h.account.cash(), 1000.0);
    EXPECT_DOUBLE_EQ(h.engine.orderManager().pendingSignedQuantity(kAlpha, "BTC"), 5.0);
    ASSERT_TRUE(h.engine.processExchangeEvent(OrderUpdate{10, kOpen, ExecutionOrderStatus::Filled}, h.persist()));
    EXPECT_DOUBLE_EQ(h.account.cash(), 1000.0);
    EXPECT_DOUBLE_EQ(h.account.positions().get("BTC"), 0.0);
    EXPECT_DOUBLE_EQ(h.engine.strategyPosition(kAlpha).get("BTC"), 0.0);
    EXPECT_TRUE(h.trades.allTrades(marks(), kOpen).empty());
}

TEST(ExecutionEngine, R8_PartialFillsApplyTheirOwnPricesAndFees)
{
    Harness<> h;
    h.manual(order());
    h.exchange.acceptOrder(10, kOpen);
    h.exchange.fillOrder(10, 2.0, 100.0, kOpen);
    h.engine.processExchangeEvents(h.persist());
    EXPECT_DOUBLE_EQ(h.account.cash(), 798.0);
    EXPECT_DOUBLE_EQ(h.account.positions().get("BTC"), 2.0);
    EXPECT_DOUBLE_EQ(h.engine.strategyPosition(kAlpha).get("BTC"), 2.0);
    EXPECT_DOUBLE_EQ(h.engine.accountEquity(marks()), 998.0);
    const auto partial = h.trades.allTrades(marks(), kOpen);
    ASSERT_EQ(partial.size(), 1U);
    EXPECT_DOUBLE_EQ(partial.front().pnl, -2.0);
    EXPECT_EQ(partial.front().fill_count, 1U);
    h.exchange.fillOrder(10, 3.0, 110.0, kLater);
    h.engine.processExchangeEvents(h.persist());
    EXPECT_DOUBLE_EQ(h.account.cash(), 464.70);
    EXPECT_DOUBLE_EQ(h.account.positions().get("BTC"), 5.0);
    EXPECT_DOUBLE_EQ(h.engine.strategyPosition(kAlpha).get("BTC"), 5.0);
    const auto complete = h.trades.allTrades(marks(110.0), kLater);
    ASSERT_EQ(complete.size(), 1U);
    EXPECT_DOUBLE_EQ(complete.front().commission, 5.30);
    EXPECT_EQ(complete.front().fill_count, 2U);
    EXPECT_DOUBLE_EQ(h.account.cash(), h.exchange.cash());
    EXPECT_DOUBLE_EQ(h.account.positions().get("BTC"), h.exchange.position("BTC"));
}

TEST(ExecutionEngine, R7_DuplicateFillDoesNotChangeCashQuantityAnalyticsOrPersistAgain)
{
    Harness<> h;
    h.manual(order());
    h.exchange.acceptOrder(10, kOpen);
    h.exchange.fillOrder(10, 2.0, 100.0, kOpen);
    h.engine.processExchangeEvents(h.persist());
    const auto persisted = h.saved.size();
    h.exchange.replayLastFill();
    h.engine.processExchangeEvents(h.persist());
    EXPECT_EQ(h.saved.size(), persisted);
    EXPECT_DOUBLE_EQ(h.account.cash(), 798.0);
    EXPECT_DOUBLE_EQ(h.account.positions().get("BTC"), 2.0);
    EXPECT_DOUBLE_EQ(h.engine.strategyPosition(kAlpha).get("BTC"), 2.0);
    EXPECT_DOUBLE_EQ(h.engine.orderManager().find(10)->filled_quantity, 2.0);
    const auto trades = h.trades.allTrades(marks(), kOpen);
    ASSERT_EQ(trades.size(), 1U);
    EXPECT_EQ(trades.front().fill_count, 1U);
    EXPECT_DOUBLE_EQ(trades.front().commission, 2.0);
}

TEST(ExecutionEngine, R8_ShortFillReceivesCashAndCreatesNegativePosition)
{
    Harness<> h;
    h.manual(order(10, OrderSide::Sell, 2.0));
    h.exchange.acceptOrder(10, kOpen);
    h.exchange.fillOrder(10, 2.0, 100.0, kOpen);
    h.engine.processExchangeEvents(h.persist());
    EXPECT_DOUBLE_EQ(h.account.cash(), 1198.0);
    EXPECT_DOUBLE_EQ(h.account.positions().get("BTC"), -2.0);
    EXPECT_DOUBLE_EQ(h.engine.strategyPosition(kAlpha).get("BTC"), -2.0);
    EXPECT_DOUBLE_EQ(h.engine.accountEquity(marks()), 998.0);
    const auto trades = h.trades.allTrades(marks(), kOpen);
    ASSERT_EQ(trades.size(), 1U);
    EXPECT_EQ(trades.front().direction, Direction::Short);
    EXPECT_DOUBLE_EQ(trades.front().pnl, -2.0);
}

TEST(ExecutionEngine, R8_ReversalClosesLongAndAttributesRemainingShortAndFees)
{
    Harness<> h;
    h.manual(order(10, OrderSide::Buy, 2.0));
    h.exchange.acceptOrder(10, kOpen);
    h.exchange.fillOrder(10, 2.0, 100.0, kOpen);
    h.engine.processExchangeEvents(h.persist());
    h.manual(order(11, OrderSide::Sell, 4.0));
    h.exchange.acceptOrder(11, kLater);
    h.exchange.fillOrder(11, 4.0, 110.0, kLater);
    h.engine.processExchangeEvents(h.persist());
    EXPECT_DOUBLE_EQ(h.account.cash(), 1233.60);
    EXPECT_DOUBLE_EQ(h.account.positions().get("BTC"), -2.0);
    EXPECT_DOUBLE_EQ(h.engine.strategyPosition(kAlpha).get("BTC"), -2.0);
    ASSERT_EQ(h.trades.closedTrades().size(), 1U);
    const auto& closed = h.trades.closedTrades().front();
    EXPECT_EQ(closed.direction, Direction::Long);
    EXPECT_DOUBLE_EQ(closed.pnl, 15.80);
    EXPECT_DOUBLE_EQ(closed.commission, 4.20);
    const auto all = h.trades.allTrades(marks(110.0), kLater);
    ASSERT_EQ(all.size(), 2U);
    EXPECT_EQ(all.back().direction, Direction::Short);
    EXPECT_DOUBLE_EQ(all.back().commission, 2.20);
}

TEST(ExecutionEngine, R9_OpposingStrategiesNetAccountUnitsWithoutLosingAttribution)
{
    Harness<> h;
    h.manual(order(10, OrderSide::Buy, 2.0, kAlpha));
    h.manual(order(11, OrderSide::Sell, 2.0, kBeta));
    h.exchange.acceptOrder(10, kOpen);
    h.exchange.acceptOrder(11, kOpen);
    h.exchange.fillOrder(10, 2.0, 100.0, kOpen);
    h.exchange.fillOrder(11, 2.0, 100.0, kOpen);
    h.engine.processExchangeEvents(h.persist());
    EXPECT_DOUBLE_EQ(h.account.cash(), 996.0);
    EXPECT_DOUBLE_EQ(h.account.positions().get("BTC"), 0.0);
    EXPECT_DOUBLE_EQ(h.engine.strategyPosition(kAlpha).get("BTC"), 2.0);
    EXPECT_DOUBLE_EQ(h.engine.strategyPosition(kBeta).get("BTC"), -2.0);
    const auto trades = h.trades.allTrades(marks(), kOpen);
    ASSERT_EQ(trades.size(), 2U);
    EXPECT_NE(trades[0].strategy_id, trades[1].strategy_id);
    EXPECT_TRUE(h.trades.closedTrades().empty());
}

TEST(ExecutionEngine, R3_LateFillAfterCancelChangesMoneyWithoutReopeningRemainder)
{
    Harness<> h;
    const auto request = order();
    h.manual(request);
    h.exchange.acceptOrder(10, kOpen);
    h.exchange.fillOrder(10, 2.0, 100.0, kOpen);
    h.engine.processExchangeEvents(h.persist());
    auto cancel = plan({}, 11);
    cancel.cancel_order_ids = {10};
    h.engine.applyOrderPlan(cancel, h.persist());
    h.exchange.confirmCancel(10, kOpen);
    h.engine.processExchangeEvents(h.persist());
    auto late = fill(request, 1.0, 110.0, 1.1, 102);
    late.timestamp = kLater;
    ASSERT_TRUE(h.engine.processExchangeEvent(late, h.persist()));
    EXPECT_DOUBLE_EQ(h.account.cash(), 686.90);
    EXPECT_DOUBLE_EQ(h.account.positions().get("BTC"), 3.0);
    EXPECT_DOUBLE_EQ(h.engine.strategyPosition(kAlpha).get("BTC"), 3.0);
    EXPECT_EQ(h.engine.orderManager().find(10)->status, ExecutionOrderStatus::Canceled);
    EXPECT_DOUBLE_EQ(h.engine.orderManager().pendingSignedQuantity(kAlpha, "BTC"), 0.0);
}

TEST(ExecutionEngine, R10_QueuedAcceptancePartialAndFinalFillsPreserveOrderedEconomics)
{
    Harness<> h;
    h.manual(order());
    h.exchange.acceptOrder(10, kOpen);
    const auto first = h.exchange.fillOrder(10, 2.0, 100.0, kOpen);
    const auto second = h.exchange.fillOrder(10, 3.0, 110.0, kLater);
    h.exchange.replayLastFill();
    h.engine.processExchangeEvents(h.persist());
    EXPECT_DOUBLE_EQ(h.account.cash(), 464.70);
    EXPECT_DOUBLE_EQ(h.account.positions().get("BTC"), 5.0);
    EXPECT_DOUBLE_EQ(h.engine.strategyPosition(kAlpha).get("BTC"), 5.0);
    EXPECT_EQ(h.engine.orderManager().find(10)->status, ExecutionOrderStatus::Filled);
    std::vector<FillID> committed;
    for (const auto& saved : h.saved)
        if (saved.new_fill) committed.push_back(saved.new_fill->fill_id);
    EXPECT_EQ(committed, (std::vector<FillID>{first.fill_id, second.fill_id}));
}

TEST(ExecutionEngine, R4_StaleStatusEchoReturnsFalseWithoutPersisting)
{
    Harness<> h;
    h.manual(order());
    ASSERT_TRUE(h.engine.processExchangeEvent(fill(order()), h.persist()));
    const auto committed = h.saved.size();
    EXPECT_FALSE(h.engine.processExchangeEvent(OrderUpdate{10, kLater, ExecutionOrderStatus::Accepted}, h.persist()));
    EXPECT_EQ(h.saved.size(), committed);
    EXPECT_DOUBLE_EQ(h.account.cash(), 798.0);
    EXPECT_EQ(h.engine.orderManager().find(10)->updated_at, kOpen);
}

TEST(ExecutionEngine, R8_FillPersistenceObservesMatchingAccountStrategyAndAuditState)
{
    Harness<> h;
    h.manual(order());
    bool sawFill = false;
    ASSERT_TRUE(h.engine.processExchangeEvent(fill(order()), [&](const std::optional<Fill>& incoming) {
        ASSERT_TRUE(incoming.has_value());
        sawFill = true;
        EXPECT_EQ(incoming->fill_id, 101U);
        EXPECT_DOUBLE_EQ(incoming->commission, 2.0);
        EXPECT_DOUBLE_EQ(h.account.cash(), 798.0);
        EXPECT_DOUBLE_EQ(h.account.positions().get("BTC"), 2.0);
        EXPECT_DOUBLE_EQ(h.engine.strategyPosition(kAlpha).get("BTC"), 2.0);
        EXPECT_DOUBLE_EQ(h.engine.orderManager().find(10)->filled_quantity, 2.0);
        EXPECT_EQ(h.engine.orderManager().processedFillIds(), std::vector<FillID>{101});
        const auto trades = h.trades.allTrades(marks(), kOpen);
        ASSERT_EQ(trades.size(), 1U);
        EXPECT_DOUBLE_EQ(trades.front().commission, 2.0);
        h.capture(incoming);
    }));
    EXPECT_TRUE(sawFill);
}

TEST(ExecutionEngine, R13_FailedFillPersistencePropagatesAndRestartCanRedeliverOnce)
{
    Harness<> original;
    original.manual(order());
    ASSERT_FALSE(original.saved.empty());
    const SavedState committed = original.saved.back();
    EXPECT_THROW(original.engine.processExchangeEvent(fill(order()), [](const std::optional<Fill>&) {
        throw std::runtime_error("Injected fill commit failure");
    }), std::runtime_error);
    Harness<> restarted;
    restarted.restore(committed);
    ASSERT_TRUE(restarted.engine.processExchangeEvent(fill(order()), restarted.persist()));
    ASSERT_TRUE(restarted.engine.processExchangeEvent(fill(order()), restarted.persist()));
    EXPECT_DOUBLE_EQ(restarted.account.cash(), 798.0);
    EXPECT_DOUBLE_EQ(restarted.account.positions().get("BTC"), 2.0);
    EXPECT_DOUBLE_EQ(restarted.engine.strategyPosition(kAlpha).get("BTC"), 2.0);
    EXPECT_EQ(restarted.saved.size(), 1U);
}

TEST(ExecutionEngine, R6_InvalidFillsCannotChangeEconomicProjectionsOrPersist)
{
    for (int invalid = 0; invalid < 6; ++invalid) {
        SCOPED_TRACE(invalid);
        Harness<> h;
        h.manual(order());
        auto bad = fill(order());
        if (invalid == 0) bad.strategy_id = kBeta;
        if (invalid == 1) bad.coin = "ETH";
        if (invalid == 2) bad.side = OrderSide::Sell;
        if (invalid == 3) bad.quantity = 6.0;
        if (invalid == 4) bad.order_id = 99;
        if (invalid == 5) bad.price = 0.0;
        const auto committed = h.saved.size();
        EXPECT_THROW(h.engine.processExchangeEvent(bad, h.persist()), std::exception);
        EXPECT_EQ(h.saved.size(), committed);
        EXPECT_DOUBLE_EQ(h.account.cash(), 1000.0);
        EXPECT_DOUBLE_EQ(h.account.positions().get("BTC"), 0.0);
        EXPECT_DOUBLE_EQ(h.engine.strategyPosition(kAlpha).get("BTC"), 0.0);
        EXPECT_DOUBLE_EQ(h.engine.orderManager().find(10)->filled_quantity, 0.0);
        EXPECT_TRUE(h.trades.allTrades(marks(), kOpen).empty());
    }
}

} // namespace
