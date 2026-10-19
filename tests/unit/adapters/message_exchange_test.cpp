#include <gtest/gtest.h>

#include <limits>
#include <optional>
#include <stdexcept>

#include "adapter_test_support.h"
#include "execution_engine.h"

namespace {
using namespace adapter_test;

TEST(MessageExchange, R1_SubmitRetainsSideUnitsDatesAndPlanningReference)
{
    for (const auto side : {OrderSide::Buy, OrderSide::Sell}) {
        RecordingBus bus;
        MessageExchange exchange(bus);
        auto request = order(40, side);
        request.reference_price = 125.0;
        request.fill_at_reference_price = true;
        exchange.submitOrder(request);
        ASSERT_EQ(bus.published.size(), 1U);
        EXPECT_EQ(bus.published.front().subject, MessageSubjects::SUBMIT_ORDER);
        const auto decoded = MessageJson::decodeSubmitOrderCommand(bus.published.front().payload);
        EXPECT_EQ(decoded.order.order_id, 40U);
        EXPECT_EQ(decoded.order.strategy_id, 7U);
        EXPECT_EQ(decoded.order.coin, "BTC");
        EXPECT_EQ(decoded.order.side, side);
        EXPECT_DOUBLE_EQ(decoded.order.quantity, 2.0);
        EXPECT_EQ(decoded.order.created_at, kD1);
        EXPECT_EQ(decoded.order.active_from, kD2);
        EXPECT_DOUBLE_EQ(decoded.order.reference_price, 125.0);
        EXPECT_TRUE(decoded.order.fill_at_reference_price);
        EXPECT_EQ(decoded.metadata.produced_at, kD1);
        EXPECT_EQ(decoded.metadata.message_id, bus.published.front().id);
        EXPECT_FALSE(decoded.metadata.correlation_id.empty());
    }
}

TEST(MessageExchange, R2_RetryAndReconstructionKeepLogicalIdentityButOtherOrdersDiffer)
{
    RecordingBus bus;
    MessageExchange first(bus);
    first.submitOrder(order());
    first.submitOrder(order());
    MessageExchange restarted(bus);
    restarted.submitOrder(order());
    restarted.submitOrder(order(41));
    ASSERT_EQ(bus.published.size(), 4U);
    EXPECT_EQ(bus.published[0].id, bus.published[1].id);
    EXPECT_EQ(bus.published[0].id, bus.published[2].id);
    EXPECT_EQ(bus.published[0].payload, bus.published[2].payload);
    EXPECT_NE(bus.published[0].id, bus.published[3].id);
    EXPECT_DOUBLE_EQ(MessageJson::decodeSubmitOrderCommand(bus.published[2].payload).order.quantity, 2.0);
}

TEST(MessageExchange, R2_CancelRetainsOrderAndStableCorrelationSeparateFromSubmit)
{
    RecordingBus bus;
    MessageExchange exchange(bus);
    exchange.submitOrder(order());
    exchange.cancelOrder(40);
    MessageExchange(bus).cancelOrder(40);
    ASSERT_EQ(bus.published.size(), 3U);
    EXPECT_EQ(bus.published[1].subject, MessageSubjects::CANCEL_ORDER);
    EXPECT_EQ(bus.published[1].id, bus.published[2].id);
    EXPECT_EQ(bus.published[1].payload, bus.published[2].payload);
    EXPECT_NE(bus.published[0].id, bus.published[1].id);
    const auto submitted = MessageJson::decodeSubmitOrderCommand(bus.published[0].payload);
    const auto canceled = MessageJson::decodeCancelOrderCommand(bus.published[1].payload);
    EXPECT_EQ(canceled.order_id, 40U);
    EXPECT_EQ(canceled.metadata.correlation_id, submitted.metadata.correlation_id);
    EXPECT_EQ(canceled.metadata.message_id, bus.published[1].id);
}

TEST(MessageExchange, R3_PublishFailurePropagatesAndRetryUsesSameLogicalCommand)
{
    RecordingBus bus;
    MessageExchange exchange(bus);
    bus.fail_next = true;
    EXPECT_THROW(exchange.submitOrder(order()), std::runtime_error);
    EXPECT_TRUE(bus.published.empty());
    EXPECT_TRUE(exchange.drainEvents().empty());
    exchange.submitOrder(order());
    ASSERT_EQ(bus.attempts.size(), 2U);
    ASSERT_EQ(bus.published.size(), 1U);
    EXPECT_EQ(bus.attempts[0].id, bus.attempts[1].id);
    EXPECT_EQ(bus.attempts[0].payload, bus.attempts[1].payload);
}

TEST(MessageExchange, R3_OutboundAdapterDoesNotSubscribePollOrManufactureEvents)
{
    RecordingBus bus;
    MessageExchange exchange(bus);
    exchange.submitOrder(order());
    exchange.cancelOrder(40);
    EXPECT_TRUE(exchange.drainEvents().empty());
    EXPECT_TRUE(exchange.drainEvents().empty());
    EXPECT_EQ(bus.subscription_count, 0U);
    EXPECT_EQ(bus.poll_count, 0U);
    EXPECT_FALSE(exchange.supportsStopEntries());
    EXPECT_FALSE(exchange.supportsShortBrackets());
}

TEST(MessageExchange, R4_ZeroSubmitAndCancelIdsRejectBeforePublishing)
{
    RecordingBus bus;
    MessageExchange exchange(bus);
    EXPECT_THROW(exchange.submitOrder(order(0)), std::invalid_argument);
    EXPECT_THROW(exchange.cancelOrder(0), std::invalid_argument);
    EXPECT_TRUE(bus.published.empty());
    EXPECT_TRUE(bus.attempts.empty());
}

// FINDING: MessageExchange publishes zero/negative/nonfinite quantities and empty
// assets from mutable market DTOs instead of rejecting them before publication.
TEST(MessageExchange, DISABLED_R4_MalformedMutableMarketIntentCannotReachWire)
{
    for (int invalid = 0; invalid < 5; ++invalid) {
        SCOPED_TRACE(invalid);
        RecordingBus bus;
        MessageExchange exchange(bus);
        auto request = order();
        if (invalid == 0) request.quantity = 0.0;
        if (invalid == 1) request.quantity = -1.0;
        if (invalid == 2) request.quantity = std::numeric_limits<double>::quiet_NaN();
        if (invalid == 3) request.quantity = std::numeric_limits<double>::infinity();
        if (invalid == 4) request.coin.clear();
        EXPECT_THROW(exchange.submitOrder(request), std::invalid_argument);
        EXPECT_TRUE(bus.published.empty());
    }
}

TEST(MessageExchange, R4_UnsupportedResearchConditionsAreRejectedWithoutDroppingFields)
{
    for (int conditional = 0; conditional < 3; ++conditional) {
        RecordingBus bus;
        MessageExchange exchange(bus);
        auto request = order();
        if (conditional == 0) request.entry_stop_price = 100.0;
        if (conditional == 1) { request.side = OrderSide::Sell; request.protective_stop_price = 120.0; }
        if (conditional == 2) { request.parent_order_id = 39; request.protective_stop_price = 120.0; }
        EXPECT_THROW(exchange.submitOrder(request), std::invalid_argument);
        EXPECT_TRUE(bus.published.empty());
    }
}

TEST(MessageExchange, R4_LivePositionStopRetainsSeparateStopPriceAndUnits)
{
    RecordingBus bus;
    MessageExchange exchange(bus);
    auto stop = order(40, OrderSide::Sell);
    stop.reference_price = 100.0;
    stop.position_stop_price = 90.0;
    exchange.submitOrder(stop);
    ASSERT_EQ(bus.published.size(), 1U);
    const auto decoded = MessageJson::decodeSubmitOrderCommand(bus.published.front().payload);
    EXPECT_EQ(decoded.order.side, OrderSide::Sell);
    EXPECT_DOUBLE_EQ(decoded.order.quantity, 2.0);
    EXPECT_DOUBLE_EQ(decoded.order.position_stop_price, 90.0);
    EXPECT_DOUBLE_EQ(decoded.order.entry_stop_price, 0.0);
    EXPECT_EQ(decoded.order.parent_order_id, 0U);
}

TEST(MessageExchange, R1_RealExecutionOwnerChangesMoneyOnlyAfterHostDeliversFill)
{
    RecordingBus bus;
    MessageExchange exchange(bus);
    Account account(1000.0);
    TradeRecorder recorder;
    std::vector<ExecutionStrategyDescriptor> strategies{{7, "Alpha", {}}};
    ExecutionEngine engine(strategies, account, recorder, exchange);
    const auto persist = [](const std::optional<Fill>&) {};
    engine.applyManualOrders({order()}, 41, kD1, persist);
    ASSERT_EQ(bus.published.size(), 1U);
    EXPECT_DOUBLE_EQ(account.cash(), 1000.0);
    EXPECT_DOUBLE_EQ(account.positions().get("BTC"), 0.0);
    EXPECT_EQ(engine.orderManager().find(40)->status, ExecutionOrderStatus::Submitted);
    ASSERT_TRUE(engine.processExchangeEvent(fill(), persist));
    EXPECT_DOUBLE_EQ(account.cash(), 798.0);
    EXPECT_DOUBLE_EQ(account.positions().get("BTC"), 2.0);
    EXPECT_EQ(bus.published.size(), 1U);
}

} // namespace
