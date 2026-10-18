#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "execution_order.h"
#include "position_state.h"


// LIVE long stop-loss (user decisions 2026-10-17, docs/codex/STOP_LOSS_DESIGN.md)
//
// Every long position of a strategy keeps one resting position stop at the venue: a
// reduce-only stop-market SELL of the whole position at (1 - fraction) x the coin's latest
// completed daily close. It is moved at every daily close (up or down) and resized when the
// position changes. A replacement is submitted first; the older stops are cancelled only once
// the newest one is accepted by the venue, so the position is never left without a stop. A
// flat position has its stops cancelled. A rejected stop is not resubmitted for the same
// price and quantity (planPositionStops reports it instead).
//
// planPositionStops is pure: it reads positions, tracked orders and closes and returns the
// stops to submit and cancel. ExecutionState applies them (ExecutionEngine::applyPositionStopChanges).
struct PositionStopChanges {
    std::vector<ExecutionOrder> submits;
    std::vector<OrderID> cancels;
    OrderID next_order_id = 0;
    std::vector<std::string> missing_closes;     // coins held without a close: no stop change
    std::vector<OrderID> rejected_stops;         // newest stop rejected for the wanted price/size
};

PositionStopChanges planPositionStops(
    const StrategyPositionSnapshot& positions,
    const std::unordered_map<OrderID, TrackedOrder>& orders,
    const std::unordered_map<Coin, double>& latestCloses,
    double fraction,
    Timestamp at,
    OrderID nextOrderId
);
