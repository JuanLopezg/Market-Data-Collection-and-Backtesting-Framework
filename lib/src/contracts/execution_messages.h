#pragma once

// Order commands, venue events and execution-time coordination messages.

#include <cstdint>
#include <unordered_map>
#include <variant>

#include "contract_metadata.h"
#include "execution_order.h"
#include "fill.h"

// Commands crossing the ExecutionEngine -> Exchange boundary.
struct SubmitOrderCommand {
    ContractMetadata metadata;
    ExecutionOrder order;
};

struct CancelOrderCommand {
    ContractMetadata metadata;
    OrderID order_id = 0;
    Timestamp requested_at = 0;
};

// Events crossing the Exchange -> ExecutionEngine boundary.
//
// Fill remains the only event allowed to mutate account/position quantities.
struct OrderUpdateEvent {
    ContractMetadata metadata;
    OrderUpdate update;
};

struct FillEvent {
    ContractMetadata metadata;
    Fill fill;
};

using ExecutionEvent = std::variant<OrderUpdateEvent, FillEvent>;

// Authoritative execution barrier emitted after one decision/open cycle
//
// The event means:
// - the order plan for decision T / execution T+1 was accepted and persisted;
// - every Fill observed for that cycle was persisted before this event;
// - there are no locally-open orders remaining when the event is emitted.
//
// It is useful to accelerated replay, but it remains an execution-domain event rather
// than a replay-specific command: the execution-state service is the only authority
// that can safely declare its own cycle quiescent.
struct ExecutionCycleComplete {
    ContractMetadata metadata;
    Timestamp decision_timestamp = 0;
    Timestamp execution_timestamp = 0;
    std::uint64_t state_revision = 0;
};

// Executable/reference prices observed at execution time (for example T+1 open)
struct ExecutionPriceSnapshot {
    ContractMetadata metadata;
    Timestamp timestamp = 0;
    Timestamp decision_timestamp = 0;
    std::unordered_map<Coin, double> prices;
};
