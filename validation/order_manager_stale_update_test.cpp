// OrderManager status updates: a redelivered/replayed status the order has already
// passed is ignored; a real contradiction still throws.
#include <cassert>
#include <iostream>
#include <stdexcept>

#include "order_manager.h"

int main()
{
    OrderManager manager;
    manager.track(ExecutionOrder(1, 1, 20261010, 20261011, "BTCUSDT", OrderSide::Buy, 0.5));
    manager.markSubmitted(1, 20261011);

    assert(manager.onOrderUpdate(OrderUpdate(1, 20261011, ExecutionOrderStatus::Accepted, "x-1", "")));
    assert(manager.onOrderUpdate(OrderUpdate(1, 20261011, ExecutionOrderStatus::Accepted, "x-1", "")));  // same status
    assert(manager.onOrderUpdate(OrderUpdate(1, 20261011, ExecutionOrderStatus::Filled, "x-1", "")));

    // Stale echoes after FILLED change nothing.
    assert(!manager.onOrderUpdate(OrderUpdate(1, 20261011, ExecutionOrderStatus::Accepted, "x-1", "")));
    assert(!manager.onOrderUpdate(OrderUpdate(1, 20261011, ExecutionOrderStatus::Submitted, "x-1", "")));
    assert(!manager.onOrderUpdate(OrderUpdate(1, 20261011, ExecutionOrderStatus::PartiallyFilled, "x-1", "")));
    assert(manager.find(1)->status == ExecutionOrderStatus::Filled);

    // A different terminal status is a contradiction, not an echo.
    bool rejected = false;
    try {
        manager.onOrderUpdate(OrderUpdate(1, 20261012, ExecutionOrderStatus::Rejected, "x-1", ""));
    } catch (const std::logic_error&) {
        rejected = true;
    }
    assert(rejected);
    assert(manager.find(1)->status == ExecutionOrderStatus::Filled);

    // ACCEPTED after PARTIALLY_FILLED is also an echo.
    manager.track(ExecutionOrder(2, 1, 20261010, 20261011, "ETHUSDT", OrderSide::Buy, 2.0));
    manager.markSubmitted(2, 20261011);
    assert(manager.onOrderUpdate(OrderUpdate(2, 20261011, ExecutionOrderStatus::PartiallyFilled, "x-2", "")));
    assert(!manager.onOrderUpdate(OrderUpdate(2, 20261011, ExecutionOrderStatus::Accepted, "x-2", "")));
    assert(manager.find(2)->status == ExecutionOrderStatus::PartiallyFilled);

    std::cout << "order manager stale-update test passed\n";
    return 0;
}
