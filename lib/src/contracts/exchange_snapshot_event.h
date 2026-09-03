#pragma once

#include "contract_metadata.h"
#include "exchange_snapshot.h"


/**************************************************************************************
 * Type    : ExchangeSnapshotEvent
 * Purpose : Durable normalized exchange truth used to reconcile execution-state
 *
 * The future exchange-gateway owns creation of this DTO. Simulation and live gateways
 * therefore drive the exact same reconciliation boundary.
 **************************************************************************************/
struct ExchangeSnapshotEvent {
    ContractMetadata metadata;
    ExchangeSnapshot snapshot;
};
