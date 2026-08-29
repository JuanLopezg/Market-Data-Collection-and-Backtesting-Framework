#pragma once

#include <variant>

#include "fill.h"
#include "order_update.h"


/**************************************************************************************
 * Type    : ExchangeEvent
 * Purpose : Ordered event stream emitted by simulated/fake/live exchange adapters
 *
 * One ordered stream avoids losing event order when ACKs, partial fills and final order
 * status updates arrive asynchronously.
 **************************************************************************************/
using ExchangeEvent = std::variant<OrderUpdate, Fill>;
