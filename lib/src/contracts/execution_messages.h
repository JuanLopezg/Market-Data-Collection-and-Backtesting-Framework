#pragma once

// Order commands, venue events and execution-time coordination messages.

#include <cstdint>
#include <optional>
#include <string>
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

// A cash movement on the venue account that is not part of a fill price. It changes cash
// only, never quantities.
enum class VenueCostKind {
    Fee,          // trading fee reported after the fill (Kraken Futures fills carry none)
    Funding,      // funding payment on an open perpetual position
    Interest,     // interest the venue charges, e.g. on a negative USD balance
    CapitalFlow   // deposit, withdrawal or conversion of USD: capital, not profit or loss
};

inline const char* venueCostKindName(VenueCostKind kind)
{
    switch (kind) {
    case VenueCostKind::Fee: return "FEE";
    case VenueCostKind::Funding: return "FUNDING";
    case VenueCostKind::Interest: return "INTEREST";
    case VenueCostKind::CapitalFlow: return "CAPITAL_FLOW";
    }
    return "UNKNOWN";
}

inline std::optional<VenueCostKind> venueCostKindFromName(const std::string& name)
{
    for (VenueCostKind kind : {VenueCostKind::Fee, VenueCostKind::Funding,
                               VenueCostKind::Interest, VenueCostKind::CapitalFlow})
        if (name == venueCostKindName(kind))
            return kind;
    return std::nullopt;
}

// Lower-case word for logs: "fee", "funding", "interest", "capital flow".
inline const char* venueCostKindLabel(VenueCostKind kind)
{
    switch (kind) {
    case VenueCostKind::Fee: return "fee";
    case VenueCostKind::Funding: return "funding";
    case VenueCostKind::Interest: return "interest";
    case VenueCostKind::CapitalFlow: return "capital flow";
    }
    return "unknown";
}

struct VenueCostEvent {
    ContractMetadata metadata;
    std::string cost_id;       // Stable venue id; each cost is applied once.
    VenueCostKind kind = VenueCostKind::Fee;
    Timestamp timestamp = 0;   // UTC business day (YYYYMMDD).
    Coin coin;                 // Source symbol; empty when the venue gave no contract.
    FillID fill_id = 0;        // Local fill a fee belongs to; 0 when not ours or unknown.
    double amount_usd = 0.0;   // Change of the account's USD cash: negative = paid, positive = received.
};

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
