#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>

#include "execution_test_support.h"

namespace {
using namespace execution_test;

TEST(ExecutionPayload, R5_OrderConstructionRejectsInvalidTradableQuantityAndAsset)
{
    for (const double invalid : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
             std::numeric_limits<double>::infinity()}) {
        EXPECT_THROW(order(10, OrderSide::Buy, invalid), std::invalid_argument);
        EXPECT_THROW(order(10, OrderSide::Sell, invalid), std::invalid_argument);
    }
    EXPECT_THROW(ExecutionOrder(10, kAlpha, kClose, kOpen, "", OrderSide::Buy, 2.0), std::invalid_argument);
}

TEST(ExecutionPayload, R5_InvalidFillNumbersFailBeforeCashPositionOrDedupMutation)
{
    for (int field = 0; field < 4; ++field) {
        for (const double invalid : {-1.0, std::numeric_limits<double>::quiet_NaN(),
                 std::numeric_limits<double>::infinity()}) {
            SCOPED_TRACE(field);
            Harness<> h;
            h.manual(order());
            auto bad = fill(order());
            if (field == 0) bad.quantity = invalid;
            if (field == 1) bad.price = invalid;
            if (field == 2) bad.commission = invalid;
            if (field == 3) bad.coin.clear();
            const auto committed = h.saved.size();
            EXPECT_THROW(h.engine.processExchangeEvent(bad, h.persist()), std::invalid_argument);
            EXPECT_EQ(h.saved.size(), committed);
            EXPECT_TRUE(h.engine.orderManager().processedFillIds().empty());
            EXPECT_DOUBLE_EQ(h.engine.orderManager().find(10)->filled_quantity, 0.0);
            EXPECT_DOUBLE_EQ(h.account.cash(), 1000.0);
            EXPECT_DOUBLE_EQ(h.account.positions().get("BTC"), 0.0);
        }
    }
    for (const bool zeroQuantity : {false, true}) {
        auto bad = fill(order());
        if (zeroQuantity) bad.quantity = 0.0;
        else bad.price = 0.0;
        EXPECT_THROW(bad.validate(), std::invalid_argument);
    }
}

TEST(ExecutionPayload, R5_ZeroCommissionFillPreservesMarkedEquity)
{
    Harness<> h;
    h.manual(order());
    ASSERT_TRUE(h.engine.processExchangeEvent(fill(order(), 2.0, 100.0, 0.0), h.persist()));
    EXPECT_DOUBLE_EQ(h.account.cash(), 800.0);
    EXPECT_DOUBLE_EQ(h.account.positions().get("BTC"), 2.0);
    EXPECT_DOUBLE_EQ(h.engine.accountEquity(marks()), 1000.0);
    const auto trades = h.trades.allTrades(marks(), kOpen);
    ASSERT_EQ(trades.size(), 1U);
    EXPECT_DOUBLE_EQ(trades.front().commission, 0.0);
    EXPECT_DOUBLE_EQ(trades.front().pnl, 0.0);
}

TEST(ExecutionPayload, R17_StopEntryRequiresPositiveTriggerAndLaterActivation)
{
    for (int invalid = 0; invalid < 5; ++invalid) {
        SCOPED_TRACE(invalid);
        auto request = order();
        request.entry_stop_price = 100.0;
        if (invalid == 0) request.entry_stop_price = -1.0;
        if (invalid == 1) request.entry_stop_price = std::numeric_limits<double>::quiet_NaN();
        if (invalid == 2) request.entry_stop_price = std::numeric_limits<double>::infinity();
        if (invalid == 3) request.active_from = kClose;
        if (invalid == 4) request.active_from = 20260104;
        EXPECT_THROW(request.validateConditional(), std::invalid_argument);
    }
}

TEST(ExecutionPayload, R17_InvalidBracketParentOrChildCannotBecomeExecutable)
{
    for (int invalid = 0; invalid < 6; ++invalid) {
        SCOPED_TRACE(invalid);
        auto child = order(11, OrderSide::Buy, 2.0);
        child.parent_order_id = 10;
        child.protective_stop_price = 120.0;
        if (invalid == 0) child.side = OrderSide::Sell;
        if (invalid == 1) child.entry_stop_price = 100.0;
        if (invalid == 2) child.protective_stop_price = 0.0;
        if (invalid == 3) child.protective_stop_price = -1.0;
        if (invalid == 4) child.active_from = 20260104;
        if (invalid == 5) child.quantity = 0.0;
        EXPECT_THROW(child.validateConditional(), std::invalid_argument);
    }
    auto unattached = order(10, OrderSide::Sell, 2.0);
    unattached.protective_stop_price = 120.0;
    EXPECT_THROW(unattached.validateConditional(), std::invalid_argument);
}

TEST(ExecutionPayload, R19_LivePositionStopsCannotMixDirectionsOrOtherOrderConditions)
{
    for (int invalid = 0; invalid < 8; ++invalid) {
        SCOPED_TRACE(invalid);
        auto stop = order(10, OrderSide::Sell, 2.0);
        stop.position_stop_price = 90.0;
        if (invalid == 0) stop.side = OrderSide::Buy;
        if (invalid == 1) stop.position_stop_price = -1.0;
        if (invalid == 2) stop.position_stop_price = std::numeric_limits<double>::quiet_NaN();
        if (invalid == 3) stop.position_stop_price = std::numeric_limits<double>::infinity();
        if (invalid == 4) stop.fill_at_reference_price = true;
        if (invalid == 5) stop.entry_stop_price = 100.0;
        if (invalid == 6) stop.parent_order_id = 8;
        if (invalid == 7) stop.quantity = 0.0;
        EXPECT_THROW(stop.validateConditional(), std::invalid_argument);
    }
}

} // namespace
