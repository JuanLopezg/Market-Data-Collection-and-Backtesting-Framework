#pragma once

#include <cmath>
#include <string>

#include <gtest/gtest.h>

#include "message_json.h"

namespace transport_test {

inline ContractMetadata metadata(const std::string& id)
{
    return {1, id, "cycle:20260102", 20260102};
}

inline ExecutionOrder orderExample()
{
    ExecutionOrder order(41, 7, 20260102, 20260103, "BTC", OrderSide::Buy, 2.0);
    order.reference_price = 125.0;
    return order;
}

inline TrackedOrder trackedExample()
{
    TrackedOrder order(ExecutionOrder(40, 7, 20260101, 20260102, "BTC", OrderSide::Buy, 5.0));
    order.status = ExecutionOrderStatus::PartiallyFilled;
    order.filled_quantity = 2.0;
    order.updated_at = 20260102;
    order.cancel_requested = true;
    order.exchange_order_id = "venue-40";
    order.last_message = "Partial: 'BTC' \"USD\"\nawaiting cancel";
    return order;
}

inline MarketDataReleaseRequest releaseExample()
{
    return {metadata("release:20260103"), MarketDataReleaseKind::ExecutionOpen, 20260103, 20260102};
}

inline MarketDataUpdated updatedExample()
{
    return {metadata("updated:20260102"), 20260102, "binance", "1d", 100, 10, 12, 14, 4, 8};
}

inline MarketSliceClosed closedExample()
{
    return {metadata("closed:20260102"), 20260102, {"BTC", "ETH"}};
}

inline MarketSliceSnapshot sliceExample()
{
    return {metadata("slice:20260102"), 20260102,
        {{"BTC", OHLCV{70.0, 80.0, 65.0, 75.0, 10.0, 720.0}},
         {"ETH", OHLCV{95.0, 110.0, 90.0, 100.0, 20.0}}}};
}

inline StrategyIntentBatch signalsExample()
{
    return {metadata("signals:20260102"), 20260102,
        {{7, "alpha \"RSI\"\nβ", {{"BTC", 1.0}, {"ETH", -0.5}}},
         {9, "idle", {}}}};
}

inline DecisionBatch decisionsExample()
{
    StrategyDecisionIntent intent;
    intent.strategy_id = 7;
    intent.decision_timestamp = 20260102;
    intent.reference_capital = 1000.0;
    intent.target_notional_usd = {{"BTC", 250.0}, {"ETH", 0.0}};
    intent.decisions = {{"BTC", RebalanceDecision::targetWeight(0.25)},
                        {"ETH", RebalanceDecision::flat()}, {"SOL", RebalanceDecision::hold()}};
    return {metadata("decision:20260102"), 20260102, {intent}};
}

inline ExecutionPriceSnapshot pricesExample()
{
    return {metadata("open:20260103"), 20260103, 20260102, {{"BTC", 125.0}, {"ETH", 100.0}}};
}

inline SubmitOrderCommand submitExample()
{
    return {metadata("submit:41"), orderExample()};
}

inline CancelOrderCommand cancelExample()
{
    return {metadata("cancel:40"), 40, 20260103};
}

inline OrderUpdateEvent updateExample()
{
    return {metadata("order-update:40"), OrderUpdate(40, 20260103,
        ExecutionOrderStatus::PartiallyFilled, "venue-40", "Partial: 'BTC' \"USD\"\nβ")};
}

inline FillEvent fillExample()
{
    Fill fill;
    fill.fill_id = 4294967297ULL;
    fill.order_id = 41;
    fill.strategy_id = 7;
    fill.timestamp = 20260103;
    fill.coin = "BTC";
    fill.side = OrderSide::Buy;
    fill.quantity = 2.0;
    fill.price = 75.0;
    fill.commission = 1.0;
    return {metadata("fill:4294967297"), fill};
}

inline VenueCostEvent costExample()
{
    return {metadata("cost:funding-1"), "funding-1", VenueCostKind::Funding,
        20260103, "BTC", 4294967297ULL, -2.5};
}

inline AccountSnapshot accountExample()
{
    AccountSnapshot account;
    account.metadata = metadata("account:20260103");
    account.timestamp = 20260103;
    account.cash = 849.0;
    account.collateral_usd = 500.0;
    account.positions = {{"BTC", 2.0}, {"ETH", -0.5}};
    account.strategy_positions = {{7, {{"BTC", 2.0}}}, {9, {{"ETH", -0.5}}}};
    account.resync_to_targets = true;
    return account;
}

inline ExecutionCycleComplete cycleExample()
{
    return {metadata("cycle:20260103"), 20260102, 20260103, 9};
}

inline ExchangeSnapshotEvent exchangeExample()
{
    ExchangeSnapshotEvent event;
    event.metadata = metadata("exchange:20260103");
    event.snapshot.timestamp = 20260103;
    event.snapshot.cash = 849.0;
    event.snapshot.collateral_usd = 500.0;
    event.snapshot.positions = {{"BTC", 2.0}, {"ETH", -0.5}};
    event.snapshot.open_orders.push_back({40, "venue-40", "BTC", OrderSide::Buy, 5.0, 2.0});
    return event;
}

inline ExchangeSnapshotRequest exchangeRequestExample()
{
    return {metadata("exchange-request:20260103")};
}

inline ExecutionPlanningStateSnapshot stateExample()
{
    return {9, {7, 9}, {{7, {{"BTC", 2.0}}}, {9, {{"ETH", -0.5}}}}, {trackedExample()}, 41};
}

inline OrderPlanningRequest quantityRequestExample()
{
    return {metadata("quantity-request:20260103"), 20260102, 20260103,
        decisionsExample(), pricesExample(), stateExample()};
}

inline OrderPlanBatch quantityPlanExample()
{
    return {metadata("quantity-plan:20260103"), 20260102, 20260103, 9,
        decisionsExample(), pricesExample(), 42, {40}, {orderExample()},
        {{"BTC", 250.0}, {"ETH", -50.0}}};
}

inline DailyCloseSnapshot closesExample()
{
    return {metadata("close:20260102"), 20260102, {{"BTC", 75.0}, {"ETH", 100.0}}};
}

inline NotionalOrderPlanningRequest notionalRequestExample()
{
    return {metadata("notional-order-plan-request:20260102:9"), 20260102,
        decisionsExample(), closesExample(), stateExample()};
}

inline NotionalOrderPlanBatch notionalPlanExample()
{
    PlannedNotionalOrder order;
    order.economic_order_id = "planned-notional-order:20260102:9:41";
    order.order_id = 41;
    order.strategy_id = 7;
    order.created_at = 20260102;
    order.decision_timestamp = 20260102;
    order.state_revision = 9;
    order.coin = "BTC";
    order.side = OrderSide::Buy;
    order.reference_close = 75.0;
    order.target_notional_usd = 250.0;
    order.current_notional_usd = 150.0;
    order.pending_notional_usd = 25.0;
    order.delta_notional_usd = 75.0;
    order.notional_usd = 75.0;
    return {metadata("notional-order-plan:20260102:9"), 20260102, 9,
        decisionsExample(), closesExample(), 42, {40}, {order}, {{"BTC", 250.0}, {"ETH", 0.0}}};
}

inline void expectSame(const ContractMetadata& a, const ContractMetadata& b)
{
    EXPECT_EQ(a.schema_version, b.schema_version);
    EXPECT_EQ(a.message_id, b.message_id);
    EXPECT_EQ(a.correlation_id, b.correlation_id);
    EXPECT_EQ(a.produced_at, b.produced_at);
}

inline void expectSame(const ExecutionOrder& a, const ExecutionOrder& b)
{
    EXPECT_EQ(a.order_id, b.order_id);
    EXPECT_EQ(a.strategy_id, b.strategy_id);
    EXPECT_EQ(a.created_at, b.created_at);
    EXPECT_EQ(a.active_from, b.active_from);
    EXPECT_EQ(a.coin, b.coin);
    EXPECT_EQ(a.side, b.side);
    EXPECT_EQ(a.quantity, b.quantity);
    EXPECT_EQ(a.entry_stop_price, b.entry_stop_price);
    EXPECT_EQ(a.protective_stop_price, b.protective_stop_price);
    EXPECT_EQ(a.parent_order_id, b.parent_order_id);
    EXPECT_EQ(a.reference_price, b.reference_price);
    EXPECT_EQ(a.fill_at_reference_price, b.fill_at_reference_price);
    EXPECT_EQ(a.position_stop_price, b.position_stop_price);
}

inline void expectSame(const TrackedOrder& a, const TrackedOrder& b)
{
    expectSame(a.request, b.request);
    EXPECT_EQ(a.status, b.status);
    EXPECT_EQ(a.filled_quantity, b.filled_quantity);
    EXPECT_EQ(a.updated_at, b.updated_at);
    EXPECT_EQ(a.cancel_requested, b.cancel_requested);
    EXPECT_EQ(a.exchange_order_id, b.exchange_order_id);
    EXPECT_EQ(a.last_message, b.last_message);
}

inline void expectSame(const MarketDataReleaseRequest& a, const MarketDataReleaseRequest& b)
{
    expectSame(a.metadata, b.metadata);
    EXPECT_EQ(a.kind, b.kind);
    EXPECT_EQ(a.timestamp, b.timestamp);
    EXPECT_EQ(a.decision_timestamp, b.decision_timestamp);
}

inline void expectSame(const MarketDataUpdated& a, const MarketDataUpdated& b)
{
    expectSame(a.metadata, b.metadata);
    EXPECT_EQ(a.completed_through, b.completed_through);
    EXPECT_EQ(a.source, b.source);
    EXPECT_EQ(a.timeframe, b.timeframe);
    EXPECT_EQ(a.ranked_symbols, b.ranked_symbols);
    EXPECT_EQ(a.active_top_n, b.active_top_n);
    EXPECT_EQ(a.tracked_symbols, b.tracked_symbols);
    EXPECT_EQ(a.maintained_symbols, b.maintained_symbols);
    EXPECT_EQ(a.requested_symbols, b.requested_symbols);
    EXPECT_EQ(a.downloaded_rows, b.downloaded_rows);
}

inline void expectSame(const MarketSliceClosed& a, const MarketSliceClosed& b)
{
    expectSame(a.metadata, b.metadata);
    EXPECT_EQ(a.timestamp, b.timestamp);
    EXPECT_EQ(a.symbols, b.symbols);
}

inline void expectSame(const MarketSliceSnapshot& a, const MarketSliceSnapshot& b)
{
    expectSame(a.metadata, b.metadata);
    EXPECT_EQ(a.timestamp, b.timestamp);
    ASSERT_EQ(a.bars.size(), b.bars.size());
    for (std::size_t i = 0; i < b.bars.size(); ++i) {
        EXPECT_EQ(a.bars[i].coin, b.bars[i].coin);
        EXPECT_EQ(a.bars[i].bar.open, b.bars[i].bar.open);
        EXPECT_EQ(a.bars[i].bar.high, b.bars[i].bar.high);
        EXPECT_EQ(a.bars[i].bar.low, b.bars[i].bar.low);
        EXPECT_EQ(a.bars[i].bar.close, b.bars[i].bar.close);
        EXPECT_EQ(a.bars[i].bar.volume, b.bars[i].bar.volume);
        if (std::isnan(b.bars[i].bar.quote_volume)) EXPECT_TRUE(std::isnan(a.bars[i].bar.quote_volume));
        else EXPECT_EQ(a.bars[i].bar.quote_volume, b.bars[i].bar.quote_volume);
    }
}

inline void expectSame(const StrategyIntentBatch& a, const StrategyIntentBatch& b)
{
    expectSame(a.metadata, b.metadata);
    EXPECT_EQ(a.timestamp, b.timestamp);
    ASSERT_EQ(a.strategies.size(), b.strategies.size());
    for (std::size_t i = 0; i < b.strategies.size(); ++i) {
        EXPECT_EQ(a.strategies[i].strategy_id, b.strategies[i].strategy_id);
        EXPECT_EQ(a.strategies[i].strategy_name, b.strategies[i].strategy_name);
        EXPECT_EQ(a.strategies[i].signals, b.strategies[i].signals);
    }
}

inline void expectSame(const DecisionBatch& a, const DecisionBatch& b)
{
    expectSame(a.metadata, b.metadata);
    EXPECT_EQ(a.decision_timestamp, b.decision_timestamp);
    ASSERT_EQ(a.strategies.size(), b.strategies.size());
    for (std::size_t i = 0; i < b.strategies.size(); ++i) {
        const auto& actual = a.strategies[i];
        const auto& expected = b.strategies[i];
        EXPECT_EQ(actual.strategy_id, expected.strategy_id);
        EXPECT_EQ(actual.decision_timestamp, expected.decision_timestamp);
        EXPECT_EQ(actual.reference_capital, expected.reference_capital);
        EXPECT_EQ(actual.target_notional_usd, expected.target_notional_usd);
        ASSERT_EQ(actual.decisions.size(), expected.decisions.size());
        for (const auto& [coin, decision] : expected.decisions) {
            ASSERT_TRUE(actual.decisions.contains(coin));
            const auto& restored = actual.decisions.at(coin);
            EXPECT_EQ(restored.action, decision.action);
            EXPECT_EQ(restored.target_weight, decision.target_weight);
            EXPECT_EQ(restored.entry_stop_price, decision.entry_stop_price);
            EXPECT_EQ(restored.protective_stop_price, decision.protective_stop_price);
        }
    }
}

inline void expectSame(const ExecutionPriceSnapshot& a, const ExecutionPriceSnapshot& b)
{
    expectSame(a.metadata, b.metadata);
    EXPECT_EQ(a.timestamp, b.timestamp);
    EXPECT_EQ(a.decision_timestamp, b.decision_timestamp);
    EXPECT_EQ(a.prices, b.prices);
}

inline void expectSame(const SubmitOrderCommand& a, const SubmitOrderCommand& b)
{
    expectSame(a.metadata, b.metadata);
    expectSame(a.order, b.order);
}

inline void expectSame(const CancelOrderCommand& a, const CancelOrderCommand& b)
{
    expectSame(a.metadata, b.metadata);
    EXPECT_EQ(a.order_id, b.order_id);
    EXPECT_EQ(a.requested_at, b.requested_at);
}

inline void expectSame(const OrderUpdateEvent& a, const OrderUpdateEvent& b)
{
    expectSame(a.metadata, b.metadata);
    EXPECT_EQ(a.update.order_id, b.update.order_id);
    EXPECT_EQ(a.update.timestamp, b.update.timestamp);
    EXPECT_EQ(a.update.status, b.update.status);
    EXPECT_EQ(a.update.exchange_order_id, b.update.exchange_order_id);
    EXPECT_EQ(a.update.message, b.update.message);
}

inline void expectSame(const FillEvent& a, const FillEvent& b)
{
    expectSame(a.metadata, b.metadata);
    EXPECT_EQ(a.fill.fill_id, b.fill.fill_id);
    EXPECT_EQ(a.fill.order_id, b.fill.order_id);
    EXPECT_EQ(a.fill.strategy_id, b.fill.strategy_id);
    EXPECT_EQ(a.fill.timestamp, b.fill.timestamp);
    EXPECT_EQ(a.fill.coin, b.fill.coin);
    EXPECT_EQ(a.fill.side, b.fill.side);
    EXPECT_EQ(a.fill.quantity, b.fill.quantity);
    EXPECT_EQ(a.fill.price, b.fill.price);
    EXPECT_EQ(a.fill.commission, b.fill.commission);
}

inline void expectSame(const VenueCostEvent& a, const VenueCostEvent& b)
{
    expectSame(a.metadata, b.metadata);
    EXPECT_EQ(a.cost_id, b.cost_id);
    EXPECT_EQ(a.kind, b.kind);
    EXPECT_EQ(a.timestamp, b.timestamp);
    EXPECT_EQ(a.coin, b.coin);
    EXPECT_EQ(a.fill_id, b.fill_id);
    EXPECT_EQ(a.amount_usd, b.amount_usd);
}

inline void expectSame(const AccountSnapshot& a, const AccountSnapshot& b)
{
    expectSame(a.metadata, b.metadata);
    EXPECT_EQ(a.timestamp, b.timestamp);
    EXPECT_EQ(a.cash, b.cash);
    EXPECT_EQ(a.collateral_usd, b.collateral_usd);
    EXPECT_EQ(a.positions, b.positions);
    EXPECT_EQ(a.strategy_positions, b.strategy_positions);
    EXPECT_EQ(a.resync_to_targets, b.resync_to_targets);
}

inline void expectSame(const ExecutionCycleComplete& a, const ExecutionCycleComplete& b)
{
    expectSame(a.metadata, b.metadata);
    EXPECT_EQ(a.decision_timestamp, b.decision_timestamp);
    EXPECT_EQ(a.execution_timestamp, b.execution_timestamp);
    EXPECT_EQ(a.state_revision, b.state_revision);
}

inline void expectSame(const ExchangeSnapshotEvent& a, const ExchangeSnapshotEvent& b)
{
    expectSame(a.metadata, b.metadata);
    EXPECT_EQ(a.snapshot.timestamp, b.snapshot.timestamp);
    EXPECT_EQ(a.snapshot.cash, b.snapshot.cash);
    EXPECT_EQ(a.snapshot.collateral_usd, b.snapshot.collateral_usd);
    EXPECT_EQ(a.snapshot.positions, b.snapshot.positions);
    ASSERT_EQ(a.snapshot.open_orders.size(), b.snapshot.open_orders.size());
    for (std::size_t i = 0; i < b.snapshot.open_orders.size(); ++i) {
        const auto& actual = a.snapshot.open_orders[i];
        const auto& expected = b.snapshot.open_orders[i];
        EXPECT_EQ(actual.local_order_id, expected.local_order_id);
        EXPECT_EQ(actual.exchange_order_id, expected.exchange_order_id);
        EXPECT_EQ(actual.coin, expected.coin);
        EXPECT_EQ(actual.side, expected.side);
        EXPECT_EQ(actual.quantity, expected.quantity);
        EXPECT_EQ(actual.filled_quantity, expected.filled_quantity);
    }
}

inline void expectSame(const ExchangeSnapshotRequest& a, const ExchangeSnapshotRequest& b)
{
    expectSame(a.metadata, b.metadata);
}

inline void expectSame(const ExecutionPlanningStateSnapshot& a, const ExecutionPlanningStateSnapshot& b)
{
    EXPECT_EQ(a.state_revision, b.state_revision);
    EXPECT_EQ(a.strategy_ids, b.strategy_ids);
    EXPECT_EQ(a.strategy_positions, b.strategy_positions);
    EXPECT_EQ(a.next_order_id, b.next_order_id);
    ASSERT_EQ(a.orders.size(), b.orders.size());
    for (std::size_t i = 0; i < b.orders.size(); ++i) expectSame(a.orders[i], b.orders[i]);
}

inline void expectSame(const OrderPlanningRequest& a, const OrderPlanningRequest& b)
{
    expectSame(a.metadata, b.metadata);
    EXPECT_EQ(a.decision_timestamp, b.decision_timestamp);
    EXPECT_EQ(a.execution_timestamp, b.execution_timestamp);
    expectSame(a.decisions, b.decisions);
    expectSame(a.prices, b.prices);
    expectSame(a.state, b.state);
}

inline void expectSame(const OrderPlanBatch& a, const OrderPlanBatch& b)
{
    expectSame(a.metadata, b.metadata);
    EXPECT_EQ(a.decision_timestamp, b.decision_timestamp);
    EXPECT_EQ(a.execution_timestamp, b.execution_timestamp);
    EXPECT_EQ(a.state_revision, b.state_revision);
    expectSame(a.decisions, b.decisions);
    expectSame(a.prices, b.prices);
    EXPECT_EQ(a.next_order_id, b.next_order_id);
    EXPECT_EQ(a.cancel_order_ids, b.cancel_order_ids);
    EXPECT_EQ(a.global_target_exposure, b.global_target_exposure);
    ASSERT_EQ(a.submit_orders.size(), b.submit_orders.size());
    for (std::size_t i = 0; i < b.submit_orders.size(); ++i) expectSame(a.submit_orders[i], b.submit_orders[i]);
}

inline void expectSame(const DailyCloseSnapshot& a, const DailyCloseSnapshot& b)
{
    expectSame(a.metadata, b.metadata);
    EXPECT_EQ(a.date, b.date);
    EXPECT_EQ(a.closes, b.closes);
}

inline void expectSame(const NotionalOrderPlanningRequest& a, const NotionalOrderPlanningRequest& b)
{
    expectSame(a.metadata, b.metadata);
    EXPECT_EQ(a.decision_timestamp, b.decision_timestamp);
    expectSame(a.decisions, b.decisions);
    expectSame(a.reference_closes, b.reference_closes);
    expectSame(a.state, b.state);
}

inline void expectSame(const NotionalOrderPlanBatch& a, const NotionalOrderPlanBatch& b)
{
    expectSame(a.metadata, b.metadata);
    EXPECT_EQ(a.decision_timestamp, b.decision_timestamp);
    EXPECT_EQ(a.state_revision, b.state_revision);
    expectSame(a.decisions, b.decisions);
    expectSame(a.reference_closes, b.reference_closes);
    EXPECT_EQ(a.next_order_id, b.next_order_id);
    EXPECT_EQ(a.cancel_order_ids, b.cancel_order_ids);
    EXPECT_EQ(a.global_target_notional_usd, b.global_target_notional_usd);
    ASSERT_EQ(a.submit_orders.size(), b.submit_orders.size());
    for (std::size_t i = 0; i < b.submit_orders.size(); ++i) {
        const auto& actual = a.submit_orders[i];
        const auto& expected = b.submit_orders[i];
        EXPECT_EQ(actual.economic_order_id, expected.economic_order_id);
        EXPECT_EQ(actual.order_id, expected.order_id);
        EXPECT_EQ(actual.strategy_id, expected.strategy_id);
        EXPECT_EQ(actual.created_at, expected.created_at);
        EXPECT_EQ(actual.decision_timestamp, expected.decision_timestamp);
        EXPECT_EQ(actual.state_revision, expected.state_revision);
        EXPECT_EQ(actual.coin, expected.coin);
        EXPECT_EQ(actual.side, expected.side);
        EXPECT_EQ(actual.reference_close, expected.reference_close);
        EXPECT_EQ(actual.target_notional_usd, expected.target_notional_usd);
        EXPECT_EQ(actual.current_notional_usd, expected.current_notional_usd);
        EXPECT_EQ(actual.pending_notional_usd, expected.pending_notional_usd);
        EXPECT_EQ(actual.delta_notional_usd, expected.delta_notional_usd);
        EXPECT_EQ(actual.notional_usd, expected.notional_usd);
    }
}

} // namespace transport_test
