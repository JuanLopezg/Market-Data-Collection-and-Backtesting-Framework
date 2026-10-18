// Offline position-stop matching: no market feed, database or real account required.
#include <cassert>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <variant>

#include "account.h"
#include "adapters/simulated_exchange.h"

namespace {

constexpr Timestamp DAY = 20261017;

ExecutionOrder stop(OrderID id, double price = 70, double quantity = 10, const Coin& coin = "BTC")
{
    ExecutionOrder order(id, 1, DAY, DAY, coin, OrderSide::Sell, quantity);
    order.position_stop_price = price;
    order.reference_price = 100;
    return order;
}

CoinBarMap bar(double open = 100, double low = 60, const Coin& coin = "BTC")
{
    return {{coin, {open, std::max(open, 110.0), low, open, 1000}}};
}

struct Fixture {
    SimulatedExchange exchange{0.001};
    Account account{10000};

    explicit Fixture(double held = 10) { account.positions().set("BTC", held); }

    std::vector<ExchangeEvent> drain()
    {
        auto events = exchange.drainEvents();
        for (const auto& event : events) {
            if (const auto* fill = std::get_if<Fill>(&event))
                account.applyFill(*fill);
        }
        return events;
    }

    void accept(const ExecutionOrder& order)
    {
        exchange.submitOrder(order);
        const auto events = drain();
        assert(events.size() == 1);
        const auto& update = std::get<OrderUpdate>(events[0]);
        assert(update.order_id == order.order_id && update.status == ExecutionOrderStatus::Accepted);
        assert(update.timestamp == (order.entry_stop_price != 0.0 ? order.active_from : order.created_at));
    }

    std::vector<ExchangeEvent> close(Timestamp day, const CoinBarMap& bars)
    {
        exchange.processPositionStopsAtClose(day, bars, account.positions());
        return drain();
    }
};

void checkFill(const std::vector<ExchangeEvent>& events, OrderID id, double price, double quantity,
               Timestamp day = DAY)
{
    assert(events.size() == 2);
    const auto& fill = std::get<Fill>(events[0]);
    assert(fill.order_id == id && fill.side == OrderSide::Sell && fill.strategy_id == 1);
    assert(fill.timestamp == day && fill.coin == "BTC");
    assert(fill.quantity == quantity && fill.price == price);
    assert(std::abs(fill.commission - quantity * price * 0.001) < 1e-12);
    const auto& update = std::get<OrderUpdate>(events[1]);
    assert(update.order_id == id && update.timestamp == day && update.status == ExecutionOrderStatus::Filled);
}

void triggerAndGap()
{
    for (double open : {100.0, 50.0}) {
        Fixture f;
        f.accept(stop(1));
        checkFill(f.close(DAY, bar(open, std::min(open, 70.0))), 1, std::min(open, 70.0), 10);
        assert(f.exchange.activeOrders().empty() && f.account.positions().get("BTC") == 0);
        assert(f.close(DAY, bar(open, 40)).empty()); // A replay cannot fill an ended stop twice.
    }
}

void restingAndCancellation()
{
    Fixture f;
    f.accept(stop(1));
    f.exchange.processOpen(DAY, bar(50, 40));
    assert(f.drain().empty()); // A stop must never be mistaken for an ordinary market sell.
    bool refused = false;
    try { f.exchange.fillAtPrice(1, DAY, 100); } catch (const std::invalid_argument&) { refused = true; }
    assert(refused && f.exchange.activeOrders().size() == 1);
    for (Timestamp day : {DAY, DAY + 1, DAY + 2}) {
        assert(f.close(day, bar(100, 71)).empty());
        assert(f.exchange.activeOrders().size() == 1);
    }
    assert(f.close(DAY + 3, {}).empty()); // Missing coin evidence does not expire the stop.
    checkFill(f.close(DAY + 4, bar()), 1, 70, 10, DAY + 4);

    Fixture canceled;
    canceled.accept(stop(2));
    canceled.exchange.cancelOrderAt(2, DAY + 1);
    const auto events = canceled.drain();
    assert(events.size() == 1);
    assert(std::get<OrderUpdate>(events[0]).status == ExecutionOrderStatus::Canceled);
    assert(canceled.close(DAY + 1, bar()).empty());
    assert(canceled.account.positions().get("BTC") == 10);

    Fixture future;
    auto later = stop(3);
    later.created_at = DAY + 1;
    later.active_from = DAY + 1;
    future.accept(later);
    assert(future.close(DAY, bar()).empty());
    checkFill(future.close(DAY + 1, bar()), 3, 70, 10, DAY + 1);
}

void reduceOnly()
{
    Fixture capped(3);
    capped.accept(stop(1));
    checkFill(capped.close(DAY, bar()), 1, 70, 3);
    assert(capped.account.positions().get("BTC") == 0);
    assert(std::abs(capped.account.cash() - 10209.79) < 1e-9);
    for (double held : {0.0, -2.0}) {
        Fixture f(held);
        f.accept(stop(2));
        const auto events = f.close(DAY, bar());
        assert(events.size() == 1 && std::holds_alternative<OrderUpdate>(events[0]));
        const auto& update = std::get<OrderUpdate>(events[0]);
        assert(update.status == ExecutionOrderStatus::Canceled);
        assert(update.message.find("no long position") != std::string::npos);
        assert(f.account.positions().get("BTC") == held && f.account.cash() == 10000);
    }
    Fixture changed(3);
    changed.accept(stop(1));
    changed.accept(ExecutionOrder(2, 1, DAY, DAY, "BTC", OrderSide::Sell, 2));
    changed.exchange.processOpen(DAY, bar());
    changed.drain();
    checkFill(changed.close(DAY, bar()), 1, 70, 1);
    assert(changed.account.positions().get("BTC") == 0);
}

void replacementAndSimultaneousTriggers()
{
    Fixture replacement;
    replacement.accept(stop(1, 70));
    replacement.accept(stop(2, 75));
    assert(replacement.exchange.activeOrders().size() == 2);
    replacement.exchange.cancelOrder(1);
    replacement.drain();
    checkFill(replacement.close(DAY + 1, bar()), 2, 75, 10, DAY + 1);

    for (double highQuantity : {10.0, 3.0}) {
        Fixture f;
        f.accept(stop(1, 60)); // Lower stop has the smaller id: price must take precedence.
        f.accept(stop(2, 80, highQuantity));
        const auto events = f.close(DAY, bar(100, 50));
        const auto& first = std::get<Fill>(events[0]);
        assert(first.order_id == 2 && first.quantity == highQuantity && first.price == 80);
        assert(std::get<OrderUpdate>(events[1]).status == ExecutionOrderStatus::Filled);
        if (highQuantity == 10) {
            assert(events.size() == 3);
            const auto& second = std::get<OrderUpdate>(events[2]);
            assert(second.order_id == 1 && second.status == ExecutionOrderStatus::Canceled);
        }
        else {
            assert(events.size() == 4);
            const auto& second = std::get<Fill>(events[2]);
            assert(second.order_id == 1 && second.quantity == 7 && second.price == 60);
            assert(second.fill_id == first.fill_id + 1);
            assert(std::get<OrderUpdate>(events[3]).status == ExecutionOrderStatus::Filled);
        }
        assert(f.account.positions().get("BTC") == 0 && f.exchange.activeOrders().empty());
    }
    Fixture tie;
    tie.accept(stop(2));
    tie.accept(stop(1));
    const auto events = tie.close(DAY, bar());
    assert(std::get<Fill>(events[0]).order_id == 1); // Stable tie break independent of hash iteration.
}

void recoveryAndOrdinaryOrders()
{
    Fixture f;
    f.accept(stop(1));
    assert(f.close(DAY, bar(100, 90)).empty());
    SimulatedExchange recovered(0.001);
    recovered.restoreState(f.exchange.activeOrders(), f.exchange.nextFillId());
    assert(recovered.drainEvents().empty()); // Recovery must not recreate Accepted events.
    recovered.processPositionStopsAtClose(DAY + 1, bar(), f.account.positions());
    checkFill(recovered.drainEvents(), 1, 70, 10, DAY + 1);

    Fixture split;
    split.accept(stop(2));
    split.exchange.configureSplitNextFill(0.4);
    const auto fills = split.close(DAY, bar());
    assert(fills.size() == 3 && std::get<Fill>(fills[0]).quantity == 4 && std::get<Fill>(fills[1]).quantity == 6);
    assert(split.account.positions().get("BTC") == 0);

    Fixture ordinary(0);
    ordinary.accept(ExecutionOrder(3, 1, DAY, DAY + 1, "BTC", OrderSide::Buy, 2));
    assert(ordinary.close(DAY + 1, bar()).empty());
    ordinary.exchange.processOpen(DAY + 1, bar(100, 50));
    auto events = ordinary.drain();
    assert(events.size() == 2 && std::get<Fill>(events[0]).price == 100 && std::get<Fill>(events[0]).quantity == 2);
    assert(ordinary.account.positions().get("BTC") == 2);

    auto entry = ExecutionOrder(4, 1, DAY, DAY + 1, "BTC", OrderSide::Buy, 2);
    entry.entry_stop_price = 105;
    ordinary.accept(entry);
    ordinary.exchange.processOpen(DAY + 1, bar());
    assert(ordinary.drain().empty());
    ordinary.exchange.processStopEntriesAtClose(DAY + 1, bar());
    events = ordinary.drain();
    assert(events.size() == 2 && std::get<Fill>(events[0]).price == 105);

    auto child = ExecutionOrder(5, 1, DAY + 1, DAY + 1, "BTC", OrderSide::Buy, 1);
    child.parent_order_id = 4;
    child.protective_stop_price = 110;
    ordinary.accept(child);
    assert(ordinary.close(DAY + 1, bar()).empty());
    ordinary.exchange.processProtectiveStopsAtClose(DAY + 1, bar());
    events = ordinary.drain();
    assert(events.size() == 2 && std::get<Fill>(events[0]).price == 110);
}

} // namespace

int main()
{
    triggerAndGap();
    restingAndCancellation();
    reduceOnly();
    replacementAndSimultaneousTriggers();
    recoveryAndOrdinaryOrders();
    std::cout << "POSITION-STOP-SIMULATED: PASS: triggers, gaps, resting, reduce-only, replacement, priority, "
                 "cancel, recovery, split fills, markets, entries and research covers\n";
}
