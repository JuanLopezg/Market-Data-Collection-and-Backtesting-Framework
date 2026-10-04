#pragma once

#include <cmath>
#include <string>
#include <vector>

#include "data_types.h"
#include "canonical_venue_errors_v1.h"
#include "canonical_venue_identity_v1.h"

/**************************************************************************************
 * Header  : canonical_venue_orders_v1.h
 * Step    : 48 — Canonical Multi-Venue Adapter Contract v1
 * Purpose : Venue-neutral order commands, immediate operation results and lifecycle
 **************************************************************************************/
namespace VenueContracts {
namespace V1 {

using RequestId = std::string;
using ItemId = std::string;

struct RequestIdentity {
    VenueContext venue;
    RequestId request_id;       // Durable canonical dedup key.
    std::string correlation_id;
    Timestamp requested_at = 0;

    bool valid() const
    {
        return venue.valid() && !request_id.empty();
    }
};

enum class Side {
    Buy = 0,
    Sell
};

enum class TimeInForce {
    Gtc = 0,
    Ioc
};

struct LimitOrderIntent {
    OrderID local_order_id = 0;
    StrategyID strategy_id = 0;
    Timestamp created_at = 0;
    Timestamp active_from = 0;

    InstrumentIdentity instrument;
    Side side = Side::Buy;
    double quantity = 0.0;
    double limit_price = 0.0;

    TimeInForce time_in_force = TimeInForce::Gtc;
    bool post_only = false;
    bool reduce_only = false;

    // Correlation/locator only. It MUST NOT be treated as an idempotency guarantee.
    std::string client_order_id;

    bool valid() const
    {
        return local_order_id != 0 &&
               instrument.valid() &&
               std::isfinite(quantity) && quantity > 0.0 &&
               std::isfinite(limit_price) && limit_price > 0.0;
    }
};

struct SubmitOrderItem {
    ItemId item_id;
    LimitOrderIntent order;

    bool valid() const
    {
        return !item_id.empty() && order.valid();
    }
};

struct SubmitOrderBatch {
    RequestIdentity request;
    std::vector<SubmitOrderItem> items;

    bool valid() const
    {
        if (!request.valid() || items.empty())
            return false;
        for (const auto& item : items) {
            if (!item.valid())
                return false;
        }
        return true;
    }
};

struct OrderLocator {
    OrderID local_order_id = 0;
    InstrumentIdentity instrument;
    NativeReferences native_references;

    bool valid() const
    {
        return local_order_id != 0 && instrument.valid();
    }
};

struct CancelOrderItem {
    ItemId item_id;
    OrderLocator order;

    bool valid() const
    {
        return !item_id.empty() && order.valid();
    }
};

struct CancelOrderBatch {
    RequestIdentity request;
    std::vector<CancelOrderItem> items;

    bool valid() const
    {
        if (!request.valid() || items.empty())
            return false;
        for (const auto& item : items) {
            if (!item.valid())
                return false;
        }
        return true;
    }
};

struct ModifyOrderItem {
    ItemId item_id;
    OrderLocator order;
    LimitOrderIntent replacement;

    bool valid() const
    {
        return !item_id.empty() &&
               order.valid() &&
               replacement.valid() &&
               order.local_order_id == replacement.local_order_id;
    }
};

struct ModifyOrderBatch {
    RequestIdentity request;
    std::vector<ModifyOrderItem> items;

    bool valid() const
    {
        if (!request.valid() || items.empty())
            return false;
        for (const auto& item : items) {
            if (!item.valid())
                return false;
        }
        return true;
    }
};

enum class CommandKind {
    Submit = 0,
    Cancel,
    Modify
};

enum class ResultScope {
    Operation = 0,
    Item
};

struct ItemResult {
    ItemId item_id;
    bool accepted = false;
    Error error;
    NativeReferences native_references;

    bool valid() const
    {
        if (item_id.empty())
            return false;
        if (accepted)
            return error.none();
        return !error.none();
    }
};

/**************************************************************************************
 * Operation-level errors and item-level errors are deliberately distinct.
 *
 * Operation rejection:
 *   scope=Operation, accepted=false, operation_error!=NONE, item_results empty.
 *
 * Item evaluation:
 *   scope=Item, accepted=true, operation_error=NONE, at least one item_result.
 **************************************************************************************/
struct OperationResult {
    RequestIdentity request;
    CommandKind command = CommandKind::Submit;
    ResultScope scope = ResultScope::Operation;
    bool accepted = false;
    Error operation_error;
    std::vector<ItemResult> item_results;

    bool valid() const
    {
        if (!request.valid())
            return false;

        if (scope == ResultScope::Operation)
            return !accepted && !operation_error.none() && item_results.empty();

        if (!accepted || !operation_error.none() || item_results.empty())
            return false;

        for (const auto& item : item_results) {
            if (!item.valid())
                return false;
        }
        return true;
    }
};

enum class OrderLifecycleStatus {
    PendingSubmit = 0,
    Accepted,
    Resting,
    PartiallyFilled,
    Filled,
    CancelPending,
    Canceled,
    Rejected,
    VenueTerminated,
    UnknownRequiresReconciliation
};

inline bool isTerminal(OrderLifecycleStatus status)
{
    return status == OrderLifecycleStatus::Filled ||
           status == OrderLifecycleStatus::Canceled ||
           status == OrderLifecycleStatus::Rejected ||
           status == OrderLifecycleStatus::VenueTerminated;
}

inline bool requiresReconciliation(OrderLifecycleStatus status)
{
    return status == OrderLifecycleStatus::UnknownRequiresReconciliation;
}

struct OrderUpdate {
    VenueContext venue;
    InstrumentIdentity instrument;
    RequestId causal_request_id;

    OrderID local_order_id = 0;
    Timestamp timestamp = 0;
    OrderLifecycleStatus status = OrderLifecycleStatus::UnknownRequiresReconciliation;

    double cumulative_filled_quantity = 0.0;
    double remaining_quantity = 0.0;

    NativeReferences native_references;
    std::string native_status;
    Error terminal_reason;

    bool valid() const
    {
        return venue.valid() &&
               instrument.valid() &&
               local_order_id != 0 &&
               std::isfinite(cumulative_filled_quantity) &&
               cumulative_filled_quantity >= 0.0 &&
               std::isfinite(remaining_quantity) &&
               remaining_quantity >= 0.0;
    }
};

inline const char* toString(OrderLifecycleStatus status)
{
    switch (status) {
    case OrderLifecycleStatus::PendingSubmit: return "PENDING_SUBMIT";
    case OrderLifecycleStatus::Accepted: return "ACCEPTED";
    case OrderLifecycleStatus::Resting: return "RESTING";
    case OrderLifecycleStatus::PartiallyFilled: return "PARTIALLY_FILLED";
    case OrderLifecycleStatus::Filled: return "FILLED";
    case OrderLifecycleStatus::CancelPending: return "CANCEL_PENDING";
    case OrderLifecycleStatus::Canceled: return "CANCELED";
    case OrderLifecycleStatus::Rejected: return "REJECTED";
    case OrderLifecycleStatus::VenueTerminated: return "VENUE_TERMINATED";
    case OrderLifecycleStatus::UnknownRequiresReconciliation:
        return "UNKNOWN_REQUIRES_RECONCILIATION";
    }
    return "UNKNOWN_REQUIRES_RECONCILIATION";
}

} // namespace V1
} // namespace VenueContracts
