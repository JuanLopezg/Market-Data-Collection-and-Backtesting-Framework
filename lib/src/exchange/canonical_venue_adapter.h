#pragma once

#include <functional>

#include "canonical_venue_account_v1.h"
#include "canonical_venue_capabilities_v1.h"
#include "canonical_venue_events_v1.h"
#include "canonical_venue_orders_v1.h"

/**************************************************************************************
 * Header  : canonical_venue_adapter.h
 * Step    : 48 — Canonical Multi-Venue Adapter Contract v1
 * Purpose : Common executable interface for MOCK, the first real venue and future venues
 *
 * This is an interface only. Step48 does not implement credentials, private routing,
 * network protocols or a concrete venue.
 *
 * Contract rules:
 * - context() identifies the explicitly configured venue/environment.
 * - capabilities() is authoritative; callers do not branch on venue_id.
 * - unsupported required capability blocks that route; no silent venue fallback.
 * - client_order_id is not proof of native idempotent submission.
 * - ambiguous submit outcomes must reconcile before any retry unless the adapter
 *   explicitly advertises IdempotentSubmit and its concrete semantics permit retry.
 * - all asynchronous output is emitted as the ordered canonical Event stream.
 **************************************************************************************/
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
