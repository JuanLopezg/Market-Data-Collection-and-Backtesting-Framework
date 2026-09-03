#pragma once

#include <variant>

#include "contract_metadata.h"
#include "fill.h"
#include "order_update.h"


/**************************************************************************************
 * Events crossing the Exchange -> ExecutionEngine boundary.
 *
 * Fill remains the only event allowed to mutate account/position quantities.
 **************************************************************************************/
struct OrderUpdateEvent {
    ContractMetadata metadata;
    OrderUpdate update;
};

struct FillEvent {
    ContractMetadata metadata;
    Fill fill;
};

using ExecutionEvent = std::variant<OrderUpdateEvent, FillEvent>;
