#include "reconciler.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_set>


namespace {

bool sameValue(double a, double b, double tolerance)
{
    return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= tolerance;
}

const ExchangeOpenOrderSnapshot* findExchangeOrder(
    const TrackedOrder& local,
    const std::vector<ExchangeOpenOrderSnapshot>& exchangeOrders,
    const std::unordered_set<std::size_t>& used,
    std::size_t& foundIndex
)
{
    for (std::size_t i = 0; i < exchangeOrders.size(); ++i) {
        if (used.find(i) != used.end())
            continue;

        const auto& exchange = exchangeOrders[i];
        if (exchange.local_order_id != 0 && exchange.local_order_id == local.request.order_id) {
            foundIndex = i;
            return &exchange;
        }
    }

    if (!local.exchange_order_id.empty()) {
        for (std::size_t i = 0; i < exchangeOrders.size(); ++i) {
            if (used.find(i) != used.end())
                continue;

            const auto& exchange = exchangeOrders[i];
            if (!exchange.exchange_order_id.empty() &&
                exchange.exchange_order_id == local.exchange_order_id) {
                foundIndex = i;
                return &exchange;
            }
        }
    }

    return nullptr;
}

}


Reconciler::Reconciler(double cashTolerance, double quantityTolerance)
    : cash_tolerance_(cashTolerance),
      quantity_tolerance_(quantityTolerance)
{
    if (!std::isfinite(cash_tolerance_) || cash_tolerance_ < 0.0)
        throw std::invalid_argument("Cash reconciliation tolerance must be finite and non-negative");
    if (!std::isfinite(quantity_tolerance_) || quantity_tolerance_ < 0.0)
        throw std::invalid_argument("Quantity reconciliation tolerance must be finite and non-negative");
}


ReconciliationReport Reconciler::compare(
    const TradingStateSnapshot& local,
    const ExchangeSnapshot& exchange
) const
{
    ReconciliationReport report;

    if (!sameValue(local.account_cash, exchange.cash, cash_tolerance_)) {
        report.issues.push_back({
            ReconciliationIssueKind::CashMismatch,
            {},
            0,
            local.account_cash,
            exchange.cash,
            "Local cash does not match exchange cash"
        });
    }

    std::unordered_set<Coin> coins;
    for (const auto& [coin, quantity] : local.account_positions) {
        (void)quantity;
        coins.insert(coin);
    }
    for (const auto& [coin, quantity] : exchange.positions) {
        (void)quantity;
        coins.insert(coin);
    }

    for (const Coin& coin : coins) {
        const auto localIt = local.account_positions.find(coin);
        const auto exchangeIt = exchange.positions.find(coin);
        const double localQuantity = localIt == local.account_positions.end() ? 0.0 : localIt->second;
        const double exchangeQuantity = exchangeIt == exchange.positions.end() ? 0.0 : exchangeIt->second;

        if (!sameValue(localQuantity, exchangeQuantity, quantity_tolerance_)) {
            report.issues.push_back({
                ReconciliationIssueKind::PositionMismatch,
                coin,
                0,
                localQuantity,
                exchangeQuantity,
                "Local filled position does not match exchange position"
            });
        }
    }

    std::unordered_set<std::size_t> matchedExchangeOrders;

    for (const TrackedOrder& localOrder : local.orders) {
        if (!localOrder.isOpen())
            continue;

        std::size_t exchangeIndex = 0;
        const auto* exchangeOrder = findExchangeOrder(
            localOrder,
            exchange.open_orders,
            matchedExchangeOrders,
            exchangeIndex
        );

        if (!exchangeOrder) {
            report.issues.push_back({
                ReconciliationIssueKind::MissingExchangeOrder,
                localOrder.request.coin,
                localOrder.request.order_id,
                localOrder.remainingQuantity(),
                0.0,
                "Locally open order is not present in exchange open orders"
            });
            continue;
        }

        matchedExchangeOrders.insert(exchangeIndex);

        const bool metadataMatches =
            exchangeOrder->coin == localOrder.request.coin &&
            exchangeOrder->side == localOrder.request.side;
        const bool quantityMatches =
            sameValue(exchangeOrder->quantity, localOrder.request.quantity, quantity_tolerance_) &&
            sameValue(exchangeOrder->filled_quantity, localOrder.filled_quantity, quantity_tolerance_);

        if (!metadataMatches || !quantityMatches) {
            report.issues.push_back({
                ReconciliationIssueKind::OrderMismatch,
                localOrder.request.coin,
                localOrder.request.order_id,
                localOrder.remainingQuantity(),
                std::max(0.0, exchangeOrder->quantity - exchangeOrder->filled_quantity),
                "Local and exchange order details differ"
            });
        }
    }

    for (std::size_t i = 0; i < exchange.open_orders.size(); ++i) {
        if (matchedExchangeOrders.find(i) != matchedExchangeOrders.end())
            continue;

        const auto& order = exchange.open_orders[i];
        report.issues.push_back({
            ReconciliationIssueKind::UnexpectedExchangeOrder,
            order.coin,
            order.local_order_id,
            0.0,
            std::max(0.0, order.quantity - order.filled_quantity),
            "Exchange has an open order not represented by local open-order state"
        });
    }

    return report;
}
