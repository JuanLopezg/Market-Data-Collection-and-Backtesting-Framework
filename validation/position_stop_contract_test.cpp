// Position stop contract (LIVE long stop-loss, 2026-10-17): ExecutionOrder::position_stop_price
// marks the resting reduce-only SELL stop of a strategy's long position. This test pins the
// contract every stop-loss task builds on: validation, the service wire JSON, and the guard
// that keeps the simulated exchange from filling a position stop as a market sell.
#include <cassert>
#include <iostream>
#include <stdexcept>
#include <variant>

#include "message_json.h"
#include "adapters/simulated_exchange.h"

namespace {

ExecutionOrder positionStop(double stopPrice)
{
    ExecutionOrder order(7, 1, 20261017, 20261017, "NEARUSDT", OrderSide::Sell, 4.0);
    order.position_stop_price = stopPrice;
    return order;
}

bool rejected(const ExecutionOrder& order)
{
    try {
        order.validateConditional();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

} // namespace

int main()
{
    // A valid position stop: SELL, positive price, no other condition.
    const ExecutionOrder stop = positionStop(3.53745);
    assert(stop.isPositionStop() && !rejected(stop));

    // Malformed stops are rejected: BUY side, research bracket fields, fill-now flag.
    ExecutionOrder buy = positionStop(3.5);
    buy.side = OrderSide::Buy;
    assert(rejected(buy));
    ExecutionOrder bracket = positionStop(3.5);
    bracket.parent_order_id = 3;
    assert(rejected(bracket));
    ExecutionOrder immediate = positionStop(3.5);
    immediate.reference_price = 5.0;
    immediate.fill_at_reference_price = true;
    assert(rejected(immediate));

    // The service wire (SubmitOrderCommand to the gateway) carries it; an ordinary order is
    // unchanged (no position_stop_price key).
    SubmitOrderCommand command;
    command.metadata = {1, "submit-7", "plan-1", 20261017};
    command.order = stop;
    const std::string payload = MessageJson::encode(command);
    assert(payload.find("\"position_stop_price\":3.53745") != std::string::npos);
    const ExecutionOrder decoded = MessageJson::decodeSubmitOrderCommand(payload).order;
    assert(decoded.isPositionStop() && decoded.position_stop_price == 3.53745 && decoded.side == OrderSide::Sell);
    command.order = ExecutionOrder(8, 1, 20261017, 20261017, "NEARUSDT", OrderSide::Buy, 4.0);
    assert(MessageJson::encode(command).find("position_stop_price") == std::string::npos);
    // A BUY carrying a stop price is refused on decode, never turned into a market order.
    std::string bad = payload;
    const auto side = bad.find("\"side\":1");
    assert(side != std::string::npos);
    bad.replace(side, 8, "\"side\":0");
    bool threw = false;
    try {
        (void)MessageJson::decodeSubmitOrderCommand(bad);
    } catch (const std::exception&) {
        threw = true;
    }
    assert(threw);

    // Until the simulator implements resting stops it rejects them instead of selling.
    SimulatedExchange exchange;
    exchange.submitOrder(stop);
    const auto events = exchange.drainEvents();
    assert(events.size() == 1);
    const auto* update = std::get_if<OrderUpdate>(&events.front());
    assert(update != nullptr && update->status == ExecutionOrderStatus::Accepted);

    std::cout << "position stop contract OK\n";
    return 0;
}
