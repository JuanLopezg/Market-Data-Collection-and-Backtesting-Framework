#pragma once

#include "contract_metadata.h"


/**************************************************************************************
 * Type    : ExchangeSnapshotRequest
 * Purpose : Request one normalized account/open-order snapshot from the exchange gateway
 *
 * The exchange/gateway side owns the snapshot timestamp. This keeps the same request
 * contract usable with a simulated clock during replay and wall-clock exchange state live.
 **************************************************************************************/
struct ExchangeSnapshotRequest {
    ContractMetadata metadata;
};
