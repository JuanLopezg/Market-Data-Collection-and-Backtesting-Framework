#pragma once

#include <variant>

#include "canonical_venue_account_v1.h"
#include "canonical_venue_orders_v1.h"

/**************************************************************************************
 * Header  : canonical_venue_events_v1.h
 * Step    : 48 — Canonical Multi-Venue Adapter Contract v1
 * Purpose : One ordered canonical event stream from any concrete venue adapter
 **************************************************************************************/
namespace VenueContracts {
namespace V1 {

using Event = std::variant<
    OperationResult,
    OrderUpdate,
    Fill,
    AccountingEvent,
    AccountSnapshot,
    OpenOrdersSnapshot,
    FillBatch
>;

} // namespace V1
} // namespace VenueContracts
