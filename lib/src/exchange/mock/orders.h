#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
#include <locale>
#include <map>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "venue_capabilities.h"
#include "venue_events.h"
#include "decimal.h"
#include "catalog.h"
#include "rules.h"

// Validate canonical order commands and maintain deterministic order lifecycle
// state for MOCK.
//
// This component decides admission, idempotency and lifecycle transitions. It does not
// decide market fills; matching is handled by MockMatching.

namespace MockVenue {

using VenueContracts::V1::CancelOrderBatch;
using VenueContracts::V1::Capability;
using VenueContracts::V1::CapabilitySet;
using VenueContracts::V1::CommandKind;
using VenueContracts::V1::Error;
using VenueContracts::V1::ErrorClass;
using VenueContracts::V1::Event;
using VenueContracts::V1::InstrumentIdentity;
using VenueContracts::V1::ItemResult;
using VenueContracts::V1::LimitOrderIntent;
using VenueContracts::V1::ModifyOrderBatch;
using VenueContracts::V1::NativeReferences;
using VenueContracts::V1::OperationResult;
using VenueContracts::V1::OrderLifecycleStatus;
using VenueContracts::V1::OrderUpdate;
using VenueContracts::V1::RequestIdentity;
using VenueContracts::V1::ResultScope;
using VenueContracts::V1::SubmitOrderBatch;
using VenueContracts::V1::TimeInForce;

struct StoredOrder {
    LimitOrderIntent intent;
    NativeReferences native_references;
    OrderLifecycleStatus status = OrderLifecycleStatus::Accepted;
    double cumulative_filled_quantity = 0.0;
    double remaining_quantity = 0.0;
    std::string admitted_request_id;

    bool active() const
    {
        return status == OrderLifecycleStatus::Accepted ||
               status == OrderLifecycleStatus::Resting ||
               status == OrderLifecycleStatus::PartiallyFilled ||
               status == OrderLifecycleStatus::CancelPending;
    }
};

class MockOrders {
public:
    using EventHandler = std::function<void(const Event&)>;

    static CapabilitySet capabilities()
    {
        return CapabilitySet{
            Capability::SubmitOrder,
            Capability::CancelOrder,
            Capability::ModifyOrder,
            Capability::ClientOrderId,
            Capability::IdempotentSubmit,
            Capability::TimeInForceGtc,
            Capability::TimeInForceIoc,
            Capability::PostOnly,
            Capability::ReduceOnly,
            Capability::BatchActions
        };
    }

    void setEventHandler(EventHandler handler) { handler_ = std::move(handler); }
    const std::vector<Event>& emittedEvents() const { return emitted_events_; }
    void clearEmittedEvents() { emitted_events_.clear(); }
    const std::map<OrderID, StoredOrder>& orders() const { return orders_; }

    const StoredOrder* findOrder(OrderID id) const
    {
        const auto it = orders_.find(id);
        return it == orders_.end() ? nullptr : &it->second;
    }

    void submit(const SubmitOrderBatch& batch)
    {
        processRequest(
            batch.request, CommandKind::Submit, fingerprint(batch),
            [&]() { return processSubmitItems(batch); });
    }

    void cancel(const CancelOrderBatch& batch)
    {
        processRequest(
            batch.request, CommandKind::Cancel, fingerprint(batch),
            [&]() { return processCancelItems(batch); });
    }

    void modify(const ModifyOrderBatch& batch)
    {
        processRequest(
            batch.request, CommandKind::Modify, fingerprint(batch),
            [&]() { return processModifyItems(batch); });
    }

    // Update lifecycle state only. This path never creates an economic fill event.
    bool applyExternalLifecycleTransition(
        OrderID local_order_id,
        Timestamp timestamp,
        OrderLifecycleStatus next_status,
        double cumulative_filled_quantity,
        double remaining_quantity,
        const std::string& native_status,
        const Error& terminal_reason = noError());

private:
    struct CachedRequest {
        std::string fingerprint;
        OperationResult operation_result;
    };

    std::map<OrderID, StoredOrder> orders_;
    std::map<std::string, OrderID> reserved_client_order_ids_;
    std::map<std::string, CachedRequest> request_cache_;

    EventHandler handler_;
    std::vector<Event> emitted_events_;
    std::vector<Event> pending_after_result_;

    static Error noError();

    static Error makeError(
        ErrorClass classification,
        const std::string& detail,
        const std::string& native_reason);

    static bool isMockContext(const RequestIdentity& request);

    static std::string doubleText(double value);

    static void appendVenue(std::ostringstream& out, const RequestIdentity& r);

    static void appendInstrument(std::ostringstream& out, const InstrumentIdentity& i);

    static void appendOrder(std::ostringstream& out, const LimitOrderIntent& o);

    static std::string fingerprint(const SubmitOrderBatch& b);

    static std::string fingerprint(const CancelOrderBatch& b);

    static std::string fingerprint(const ModifyOrderBatch& b);

    template <typename Processor>
    void processRequest(
        const RequestIdentity& request,
        CommandKind command,
        const std::string& fp,
        Processor processor)
    {
        if (!request.valid())
            return; // Invalid request identity cannot form a valid canonical result.

        // Request IDs identify the complete command payload. Repeating an identical
        // request re-emits its result without reapplying lifecycle changes.
        const auto cached = request_cache_.find(request.request_id);
        if (cached != request_cache_.end()) {
            if (cached->second.fingerprint == fp) {
                emit(cached->second.operation_result);
                return;
            }
            emit(operationFailure(
                request, command, ErrorClass::DuplicateRequest,
                "canonical request_id was already used with a different payload",
                "REQUEST_ID_PAYLOAD_CONFLICT"));
            return;
        }

        if (!isMockContext(request)) {
            OperationResult result = operationFailure(
                request, command, ErrorClass::InvalidRequest,
                "request venue/environment does not match explicit MOCK context",
                "WRONG_VENUE_CONTEXT");
            request_cache_.emplace(request.request_id, CachedRequest{fp, result});
            emit(result);
            return;
        }

        pending_after_result_.clear();
        OperationResult result = processor();
        request_cache_.emplace(request.request_id, CachedRequest{fp, result});

        // Deterministic observable ordering: command result first, lifecycle updates after.
        emit(result);
        for (const auto& event : pending_after_result_)
            emit(event);
        pending_after_result_.clear();
    }

    static OperationResult operationFailure(
        const RequestIdentity& request,
        CommandKind command,
        ErrorClass classification,
        const std::string& detail,
        const std::string& native_reason);

    static ItemResult rejectedItem(
        const std::string& item_id,
        ErrorClass classification,
        const std::string& detail,
        const std::string& native_reason);

    static ItemResult acceptedItem(
        const std::string& item_id,
        const NativeReferences& refs);

    OperationResult processSubmitItems(const SubmitOrderBatch& batch);

    ItemResult processSubmitItem(
        const RequestIdentity& request,
        const VenueContracts::V1::SubmitOrderItem& item);

    OperationResult processCancelItems(const CancelOrderBatch& batch);

    ItemResult processCancelItem(
        const RequestIdentity& request,
        const VenueContracts::V1::CancelOrderItem& item);

    OperationResult processModifyItems(const ModifyOrderBatch& batch);

    ItemResult processModifyItem(
        const RequestIdentity& request,
        const VenueContracts::V1::ModifyOrderItem& item);

    static bool sameInstrument(
        const InstrumentIdentity& a,
        const InstrumentIdentity& b);

    static Error admitOrder(const LimitOrderIntent& order);

    static bool validTransition(
        OrderLifecycleStatus from,
        OrderLifecycleStatus to);

    static OrderUpdate makeOrderUpdate(
        const StoredOrder& stored,
        Timestamp timestamp,
        const std::string& causal_request_id,
        const std::string& native_status,
        const Error& terminal_reason);

    template <typename T>
    void emit(const T& value)
    {
        Event e = value;
        emitted_events_.push_back(e);
        if (handler_)
            handler_(e);
    }
};

} // namespace MockVenue
