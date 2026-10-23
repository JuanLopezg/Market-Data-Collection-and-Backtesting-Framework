#include "position_stops.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <utility>

namespace {

constexpr double QUANTITY_EPSILON = 1e-12;

bool sameValue(double a, double b)
{
    return std::abs(a - b) <= 1e-9 * std::max(1.0, std::max(std::abs(a), std::abs(b)));
}

bool acceptedByVenue(const TrackedOrder& order)
{
    return order.status == ExecutionOrderStatus::Accepted || order.status == ExecutionOrderStatus::PartiallyFilled;
}

} // namespace


PositionStopChanges planPositionStops(
    const StrategyPositionSnapshot& positions,
    const std::unordered_map<OrderID, TrackedOrder>& orders,
    const std::unordered_map<Coin, double>& latestCloses,
    double fraction,
    Timestamp at,
    OrderID nextOrderId
)
{
    if (!std::isfinite(fraction) || fraction <= 0.0 || fraction >= 1.0)
        throw std::invalid_argument("Position stop fraction must be in (0, 1)");
    if (at == 0 || nextOrderId == 0)
        throw std::invalid_argument("Position stops need a timestamp and a next order id");

    // Every (strategy, coin) with a long position or with a position stop, in a stable order.
    std::map<std::pair<StrategyID, Coin>, double> held;
    for (const auto& [strategyId, state] : positions) {
        for (const auto& [coin, quantity] : state.values()) {
            if (quantity > QUANTITY_EPSILON)
                held[{strategyId, coin}] = quantity;
        }
    }
    std::map<std::pair<StrategyID, Coin>, std::vector<const TrackedOrder*>> stops;
    // Sells still working that are not stops (the daily plan's exits and trims).
    std::map<std::pair<StrategyID, Coin>, double> pendingSells;
    for (const auto& [orderId, order] : orders) {
        (void)orderId;
        if (order.request.isPositionStop())
            stops[{order.request.strategy_id, order.request.coin}].push_back(&order);
        else if (order.isOpen() && order.request.side == OrderSide::Sell)
            pendingSells[{order.request.strategy_id, order.request.coin}] += order.remainingQuantity();
    }
    for (auto& [key, list] : stops) {
        (void)key;
        std::sort(list.begin(), list.end(), [](const TrackedOrder* a, const TrackedOrder* b) {
            return a->request.order_id < b->request.order_id;
        });
        held.try_emplace(key, 0.0);
    }

    PositionStopChanges changes;
    changes.next_order_id = nextOrderId;
    for (const auto& [key, quantity] : held) {
        const auto& [strategyId, coin] = key;
        const std::vector<const TrackedOrder*> all = stops.count(key) ? stops.at(key) : std::vector<const TrackedOrder*>{};
        std::vector<const TrackedOrder*> working; // open and not being cancelled
        for (const TrackedOrder* order : all) {
            if (order->isOpen() && !order->cancel_requested)
                working.push_back(order);
        }

        if (quantity <= QUANTITY_EPSILON) { // flat: no stop left behind
            for (const TrackedOrder* order : working)
                changes.cancels.push_back(order->request.order_id);
            continue;
        }

        // A position this cycle is selling entirely keeps the stop it has until the sale
        // fills (then it is flat and the stop is cancelled): no new stop for a position
        // that is about to close (2026-10-23: STRK and XRP got a new stop while sold).
        if (const auto selling = pendingSells.find(key);
            selling != pendingSells.end() && selling->second >= quantity - QUANTITY_EPSILON)
            continue;

        const auto close = latestCloses.find(coin);
        if (close == latestCloses.end() || !std::isfinite(close->second) || close->second <= 0.0) {
            changes.missing_closes.push_back(coin);
            continue;
        }
        const double price = close->second * (1.0 - fraction);
        const auto matches = [&](const TrackedOrder* order) {
            return sameValue(order->request.quantity, quantity) && sameValue(order->request.position_stop_price, price);
        };

        const TrackedOrder* newest = working.empty() ? nullptr : working.back();
        if (newest && matches(newest)) {
            // The wanted stop exists; retire the older ones once the venue accepted it.
            if (acceptedByVenue(*newest)) {
                for (const TrackedOrder* order : working) {
                    if (order != newest)
                        changes.cancels.push_back(order->request.order_id);
                }
            }
            continue;
        }

        // Do not resubmit what the venue just rejected for the same price and size.
        if (!all.empty() && all.back()->status == ExecutionOrderStatus::Rejected && matches(all.back())) {
            changes.rejected_stops.push_back(all.back()->request.order_id);
            continue;
        }

        ExecutionOrder stop(changes.next_order_id++, strategyId, at, at, coin, OrderSide::Sell, quantity);
        stop.position_stop_price = price;
        stop.reference_price = close->second;
        stop.validateConditional();
        changes.submits.push_back(std::move(stop));
    }
    return changes;
}
