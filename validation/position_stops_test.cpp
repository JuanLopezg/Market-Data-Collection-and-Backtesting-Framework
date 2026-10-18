// LIVE long stop-loss lifecycle (task D, docs/codex/STOP_LOSS_DESIGN.md): which stops
// ExecutionState places and cancels (planPositionStops), how the engine applies them
// (ExecutionEngine::applyPositionStopChanges) and that the order planner never sees them.
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include "account.h"
#include "execution_engine.h"
#include "fake_exchange.h"
#include "planning/planning_state.h"
#include "position_stops.h"
#include "trade_recorder.h"

namespace {

int failures = 0;

void check(bool condition, const std::string& label)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << label << '\n';
    }
}

constexpr double FRACTION = 0.30;
constexpr Timestamp TODAY = 20261018;

StrategyPositionSnapshot longs(double near, double wld = 0.0)
{
    StrategyPositionSnapshot positions;
    positions[1].set("NEARUSDT", near);
    if (wld != 0.0)
        positions[1].set("WLDUSDT", wld);
    return positions;
}

TrackedOrder stop(OrderID id, double quantity, double price, ExecutionOrderStatus status)
{
    ExecutionOrder request(id, 1, 20261017, 20261017, "NEARUSDT", OrderSide::Sell, quantity);
    request.position_stop_price = price;
    TrackedOrder tracked(request);
    tracked.status = status;
    return tracked;
}

const std::unordered_map<Coin, double> closes{{"NEARUSDT", 5.0}, {"WLDUSDT", 0.5}};

void placesAStopForEveryLong()
{
    const auto changes = planPositionStops(longs(4.0, 43.0), {}, closes, FRACTION, TODAY, 10);
    check(changes.submits.size() == 2 && changes.cancels.empty(), "one stop per long");
    const ExecutionOrder& near = changes.submits.at(0).coin == "NEARUSDT" ? changes.submits.at(0) : changes.submits.at(1);
    check(near.side == OrderSide::Sell && near.quantity == 4.0 && std::abs(near.position_stop_price - 3.5) < 1e-12,
          "SELL the whole position at 70% of the close");
    check(near.reference_price == 5.0 && near.created_at == TODAY && near.active_from == TODAY, "sent today, close as reference");
    check(changes.next_order_id == 12, "order ids allocated");
}

void replacementCancelsTheOldStopOnlyAfterTheNewIsAccepted()
{
    // Yesterday's stop at 3.0; today's close 5.0 wants 3.5: a new stop first, the old one stays.
    std::unordered_map<OrderID, TrackedOrder> orders{{3, stop(3, 4.0, 3.0, ExecutionOrderStatus::Accepted)}};
    auto changes = planPositionStops(longs(4.0), orders, closes, FRACTION, TODAY, 10);
    check(changes.submits.size() == 1 && changes.cancels.empty(), "new stop before cancelling the old");

    // The new stop is submitted but not accepted yet: nothing more.
    orders.emplace(10, stop(10, 4.0, 3.5, ExecutionOrderStatus::Submitted));
    changes = planPositionStops(longs(4.0), orders, closes, FRACTION, TODAY, 11);
    check(changes.submits.empty() && changes.cancels.empty(), "wait for the venue to accept the new stop");

    // Accepted: the old stop is cancelled; a stop being cancelled is not cancelled twice.
    orders.at(10).status = ExecutionOrderStatus::Accepted;
    changes = planPositionStops(longs(4.0), orders, closes, FRACTION, TODAY, 11);
    check(changes.submits.empty() && changes.cancels == std::vector<OrderID>{3}, "old stop cancelled once replaced");
    orders.at(3).cancel_requested = true;
    changes = planPositionStops(longs(4.0), orders, closes, FRACTION, TODAY, 11);
    check(changes.submits.empty() && changes.cancels.empty(), "nothing left to do");
}

void followsTheCloseDownAndTheSize()
{
    std::unordered_map<OrderID, TrackedOrder> orders{{3, stop(3, 4.0, 3.5, ExecutionOrderStatus::Accepted)}};
    // The close fell to 4.0: the stop moves DOWN to 2.8 (not a ratchet).
    auto changes = planPositionStops(longs(4.0), orders, {{"NEARUSDT", 4.0}}, FRACTION, TODAY, 10);
    check(changes.submits.size() == 1 && std::abs(changes.submits.at(0).position_stop_price - 2.8) < 1e-12,
          "the stop follows a lower close");
    // A rebalance changed the size: a new stop for the new quantity.
    changes = planPositionStops(longs(6.0), orders, closes, FRACTION, TODAY, 10);
    check(changes.submits.size() == 1 && changes.submits.at(0).quantity == 6.0, "the stop follows the position size");
}

void flatPositionsCancelTheirStops()
{
    std::unordered_map<OrderID, TrackedOrder> orders{
        {3, stop(3, 4.0, 3.5, ExecutionOrderStatus::Accepted)},
        {4, stop(4, 4.0, 3.6, ExecutionOrderStatus::Submitted)},
        {2, stop(2, 4.0, 3.4, ExecutionOrderStatus::Filled)}};
    const auto changes = planPositionStops(longs(0.0), orders, closes, FRACTION, TODAY, 10);
    check(changes.submits.empty() && changes.cancels.size() == 2, "every open stop of a flat position cancelled");
}

void rejectedStopsAreNotResubmittedForTheSamePrice()
{
    std::unordered_map<OrderID, TrackedOrder> orders{{5, stop(5, 4.0, 3.5, ExecutionOrderStatus::Rejected)}};
    auto changes = planPositionStops(longs(4.0), orders, closes, FRACTION, TODAY, 10);
    check(changes.submits.empty() && changes.rejected_stops == std::vector<OrderID>{5}, "rejection reported, not retried");
    // A new close (new price) tries again.
    changes = planPositionStops(longs(4.0), orders, {{"NEARUSDT", 6.0}}, FRACTION, TODAY, 10);
    check(changes.submits.size() == 1, "a new price is tried");
}

void missingClosesLeaveTheStopAlone()
{
    std::unordered_map<OrderID, TrackedOrder> orders{{3, stop(3, 4.0, 3.5, ExecutionOrderStatus::Accepted)}};
    const auto changes = planPositionStops(longs(4.0), orders, {}, FRACTION, TODAY, 10);
    check(changes.submits.empty() && changes.cancels.empty() && changes.missing_closes == std::vector<std::string>{"NEARUSDT"},
          "no close: the existing stop stays, reported");
    bool threw = false;
    try {
        (void)planPositionStops(longs(4.0), {}, closes, 0.0, TODAY, 10);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw, "a zero fraction is not a stop policy");
}

void engineAppliesStopsAndThePlannerIgnoresThem()
{
    Account account(1000.0);
    TradeRecorder recorder;
    FakeExchange exchange(1000.0);
    ExecutionEngine engine(std::vector<ExecutionStrategyDescriptor>{{1, "Pure_RSI", {}}}, account, recorder, exchange);
    engine.restoreState(1000.0, {}, {}, {}, {}, 10, 20261017);

    ExecutionOrder first(10, 1, TODAY, TODAY, "NEARUSDT", OrderSide::Sell, 4.0);
    first.position_stop_price = 3.5;
    int saves = 0;
    engine.applyPositionStopChanges({first}, {}, 11, TODAY, [&](const std::optional<Fill>&) { ++saves; });
    check(exchange.submitCount() == 1 && engine.nextOrderId() == 11 && saves == 2, "stop staged, saved and sent");
    check(engine.lastExecutionTimestamp() == 20261017, "daily-cycle markers unchanged");

    // The planner does not see the stop: no pending quantity, no cancel candidate, and the
    // planning revision does not change when stops are replaced.
    const auto withStop = makeExecutionPlanningStateSnapshot({1}, engine.strategyPositions(), engine.orderManager(), 11);
    check(withStop.orders.empty(), "position stops are hidden from the planner");
    check(engine.orderManager().find(10)->pendingSignedQuantity() == 0.0, "no pending quantity for a stop");

    exchange.acceptOrder(10, TODAY);
    engine.processExchangeEvents([](const std::optional<Fill>&) {});
    ExecutionOrder second(11, 1, TODAY, TODAY, "NEARUSDT", OrderSide::Sell, 4.0);
    second.position_stop_price = 3.6;
    engine.applyPositionStopChanges({second}, {10}, 12, TODAY, [](const std::optional<Fill>&) {});
    check(exchange.submitCount() == 2 && exchange.cancelRequested(10), "replacement sent and old stop cancel sent");

    bool rejected = false;
    try { // only position stops, and only open stops may be cancelled
        ExecutionOrder market(12, 1, TODAY, TODAY, "NEARUSDT", OrderSide::Sell, 1.0);
        engine.applyPositionStopChanges({market}, {}, 13, TODAY, [](const std::optional<Fill>&) {});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "a market order is not accepted as a stop change");
    rejected = false;
    try {
        engine.applyPositionStopChanges({}, {10}, 12, TODAY, [](const std::optional<Fill>&) {});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "a stop already being cancelled is not cancelled twice");
}

} // namespace

int main()
{
    placesAStopForEveryLong();
    replacementCancelsTheOldStopOnlyAfterTheNewIsAccepted();
    followsTheCloseDownAndTheSize();
    flatPositionsCancelTheirStops();
    rejectedStopsAreNotResubmittedForTheSamePrice();
    missingClosesLeaveTheStopAlone();
    engineAppliesStopsAndThePlannerIgnoresThem();
    if (failures != 0) {
        std::cerr << failures << " position stop check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "position stops OK\n";
    return EXIT_SUCCESS;
}
