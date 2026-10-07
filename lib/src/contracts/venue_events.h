#pragma once

#include <variant>

#include "venue_account.h"
#include "venue_orders.h"

// One ordered canonical event stream from any concrete venue adapter
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
