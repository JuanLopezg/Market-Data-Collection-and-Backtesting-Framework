#pragma once

#include <functional>

#include "venue_account.h"
#include "venue_capabilities.h"
#include "venue_events.h"
#include "venue_orders.h"

// Common execution interface shared by MOCK and concrete trading venues.
//
// This interface defines the behavior expected by higher-level trading code. Credentials,
// private routing, network protocols and venue-specific details belong in concrete adapters.
//
// Contract rules:
// - context() identifies the explicitly configured venue/environment.
// - capabilities() is authoritative; callers do not branch on venue_id.
// - unsupported required capability blocks that route; no silent venue fallback.
// - client_order_id is not proof of native idempotent submission.
// - ambiguous submit outcomes must reconcile before any retry unless the adapter
// explicitly advertises IdempotentSubmit and its concrete semantics permit retry.
// - all asynchronous output is emitted as the ordered canonical Event stream.
class CanonicalVenueAdapter {
public:
    using EventHandler = std::function<void(const VenueContracts::V1::Event&)>;

    virtual ~CanonicalVenueAdapter() = default;

    virtual VenueContracts::V1::VenueContext context() const = 0;
    virtual VenueContracts::V1::CapabilitySet capabilities() const = 0;

    virtual void setEventHandler(EventHandler handler) = 0;

    virtual void submitOrders(
        const VenueContracts::V1::SubmitOrderBatch& request
    ) = 0;

    virtual void cancelOrders(
        const VenueContracts::V1::CancelOrderBatch& request
    ) = 0;

    virtual void modifyOrders(
        const VenueContracts::V1::ModifyOrderBatch& request
    ) = 0;

    virtual void requestAccountSnapshot(
        const VenueContracts::V1::AccountSnapshotRequest& request
    ) = 0;

    virtual void requestOpenOrders(
        const VenueContracts::V1::OpenOrdersRequest& request
    ) = 0;

    virtual void requestOrderStatus(
        const VenueContracts::V1::OrderStatusRequest& request
    ) = 0;

    virtual void requestFills(
        const VenueContracts::V1::FillBackfillRequest& request
    ) = 0;

    // Technical/process polling only. It must never be used as business/economic time.
    virtual void poll(int timeout_ms) = 0;
};
