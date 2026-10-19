#pragma once

#include <algorithm>
#include <map>
#include <set>
#include <utility>

#include <gtest/gtest.h>

#include "fill.h"
#include "trading_state_snapshot.h"

namespace persistence_test {

inline Fill exampleFill(FillID id = 101)
{
    Fill fill;
    fill.fill_id = id;
    fill.order_id = 41;
    fill.strategy_id = 7;
    fill.timestamp = 20260102;
    fill.coin = "BTC";
    fill.side = OrderSide::Buy;
    fill.quantity = 2.0;
    fill.price = 75.0;
    fill.commission = 1.0;
    return fill;
}

inline TrackedOrder exampleOrder(OrderID id = 41)
{
    TrackedOrder order(ExecutionOrder(id, 7, 20260101, 20260102, "BTC", OrderSide::Buy, 5.0));
    order.status = ExecutionOrderStatus::PartiallyFilled;
    order.filled_quantity = 2.0;
    order.updated_at = 20260102;
    order.cancel_requested = true;
    order.exchange_order_id = "venue-41";
    order.last_message = "Partial fill; cancel pending: 'BTC' \"USD\"";
    return order;
}

inline TradingStateSnapshot exampleSnapshot()
{
    TradingStateSnapshot snapshot;
    snapshot.last_bar_close_timestamp = 20260102;
    snapshot.last_execution_timestamp = 20260102;
    snapshot.next_order_id = 42;
    snapshot.account_cash = 849.0;
    snapshot.account_positions = {{"BTC", 2.0}, {"ETH", -0.5}};
    snapshot.strategies = {
        {7, {{"BTC", 1.0}}, {{"BTC", 0.25}}, {{"BTC", 2.0}}},
        {9, {{"ETH", -1.0}}, {{"ETH", -0.125}}, {{"ETH", -0.5}}}
    };
    RebalancePlan plan(20260102, 1000.0);
    plan.set("BTC", RebalanceDecision::targetWeight(0.25));
    plan.set("ETH", RebalanceDecision::flat());
    snapshot.pending_plans.push_back({7, std::move(plan)});
    snapshot.orders.push_back(exampleOrder());
    snapshot.processed_fill_ids = {101, 99};
    return snapshot;
}

inline void expectFill(const Fill& actual, const Fill& expected)
{
    EXPECT_EQ(actual.fill_id, expected.fill_id);
    EXPECT_EQ(actual.order_id, expected.order_id);
    EXPECT_EQ(actual.strategy_id, expected.strategy_id);
    EXPECT_EQ(actual.timestamp, expected.timestamp);
    EXPECT_EQ(actual.coin, expected.coin);
    EXPECT_EQ(actual.side, expected.side);
    EXPECT_EQ(actual.quantity, expected.quantity);
    EXPECT_EQ(actual.price, expected.price);
    EXPECT_EQ(actual.commission, expected.commission);
}

inline void expectOrder(const TrackedOrder& actual, const TrackedOrder& expected)
{
    EXPECT_EQ(actual.request.order_id, expected.request.order_id);
    EXPECT_EQ(actual.request.strategy_id, expected.request.strategy_id);
    EXPECT_EQ(actual.request.created_at, expected.request.created_at);
    EXPECT_EQ(actual.request.active_from, expected.request.active_from);
    EXPECT_EQ(actual.request.coin, expected.request.coin);
    EXPECT_EQ(actual.request.side, expected.request.side);
    EXPECT_EQ(actual.request.quantity, expected.request.quantity);
    EXPECT_EQ(actual.request.entry_stop_price, expected.request.entry_stop_price);
    EXPECT_EQ(actual.request.protective_stop_price, expected.request.protective_stop_price);
    EXPECT_EQ(actual.request.parent_order_id, expected.request.parent_order_id);
    EXPECT_EQ(actual.request.reference_price, expected.request.reference_price);
    EXPECT_EQ(actual.request.fill_at_reference_price, expected.request.fill_at_reference_price);
    EXPECT_EQ(actual.request.position_stop_price, expected.request.position_stop_price);
    EXPECT_EQ(actual.status, expected.status);
    EXPECT_EQ(actual.filled_quantity, expected.filled_quantity);
    EXPECT_EQ(actual.updated_at, expected.updated_at);
    EXPECT_EQ(actual.cancel_requested, expected.cancel_requested);
    EXPECT_EQ(actual.exchange_order_id, expected.exchange_order_id);
    EXPECT_EQ(actual.last_message, expected.last_message);
}

// Durable collections are keyed by their stable IDs; serialization need not retain
// the iteration order of maps or vectors assembled from maps.
inline void expectSnapshot(const TradingStateSnapshot& actual, const TradingStateSnapshot& expected)
{
    EXPECT_EQ(actual.schema_version, expected.schema_version);
    EXPECT_EQ(actual.last_bar_close_timestamp, expected.last_bar_close_timestamp);
    EXPECT_EQ(actual.last_execution_timestamp, expected.last_execution_timestamp);
    EXPECT_EQ(actual.next_order_id, expected.next_order_id);
    EXPECT_EQ(actual.account_cash, expected.account_cash);
    EXPECT_EQ(actual.account_positions, expected.account_positions);
    EXPECT_EQ(std::set<FillID>(actual.processed_fill_ids.begin(), actual.processed_fill_ids.end()),
              std::set<FillID>(expected.processed_fill_ids.begin(), expected.processed_fill_ids.end()));
    EXPECT_EQ(actual.processed_fill_ids.size(), expected.processed_fill_ids.size());

    ASSERT_EQ(actual.strategies.size(), expected.strategies.size());
    for (const auto& strategy : expected.strategies) {
        SCOPED_TRACE(strategy.strategy_id);
        const auto found = std::find_if(actual.strategies.begin(), actual.strategies.end(),
            [&](const auto& candidate) { return candidate.strategy_id == strategy.strategy_id; });
        ASSERT_NE(found, actual.strategies.end());
        EXPECT_EQ(found->signals, strategy.signals);
        EXPECT_EQ(found->desired_weights, strategy.desired_weights);
        EXPECT_EQ(found->virtual_positions, strategy.virtual_positions);
    }

    ASSERT_EQ(actual.pending_plans.size(), expected.pending_plans.size());
    for (const auto& pending : expected.pending_plans) {
        const auto found = std::find_if(actual.pending_plans.begin(), actual.pending_plans.end(),
            [&](const auto& candidate) { return candidate.strategy_id == pending.strategy_id; });
        ASSERT_NE(found, actual.pending_plans.end());
        EXPECT_EQ(found->plan.timestamp(), pending.plan.timestamp());
        EXPECT_EQ(found->plan.referenceCapital(), pending.plan.referenceCapital());
        ASSERT_EQ(found->plan.size(), pending.plan.size());
        for (const auto& [coin, decision] : pending.plan.values()) {
            ASSERT_TRUE(found->plan.contains(coin));
            const auto restored = found->plan.get(coin);
            EXPECT_EQ(restored.action, decision.action);
            EXPECT_EQ(restored.target_weight, decision.target_weight);
            EXPECT_EQ(restored.entry_stop_price, decision.entry_stop_price);
            EXPECT_EQ(restored.protective_stop_price, decision.protective_stop_price);
        }
    }

    ASSERT_EQ(actual.orders.size(), expected.orders.size());
    for (const auto& order : expected.orders) {
        const auto found = std::find_if(actual.orders.begin(), actual.orders.end(),
            [&](const auto& candidate) { return candidate.request.order_id == order.request.order_id; });
        ASSERT_NE(found, actual.orders.end());
        expectOrder(*found, order);
    }
}

} // namespace persistence_test
