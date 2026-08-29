#pragma once

#include "price_snapshot.h"


/**************************************************************************************
 * Type    : ExecutionReferencePrices
 * Purpose : Prices used immediately before order planning to resolve monetary targets
 *           into quantities
 *
 * These are reference prices only. The exchange remains responsible for the actual
 * fill price.
 **************************************************************************************/
using ExecutionReferencePrices = PriceSnapshot;
