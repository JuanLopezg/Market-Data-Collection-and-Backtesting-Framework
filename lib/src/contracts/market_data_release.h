#pragma once

#include "contract_metadata.h"


/**************************************************************************************
 * Type    : MarketDataReleaseKind
 * Purpose : Replay-control phase released by the historical market-data service
 **************************************************************************************/
enum class MarketDataReleaseKind : int {
    ClosedSlice = 1,
    ExecutionOpen = 2
};


/**************************************************************************************
 * Type    : MarketDataReleaseRequest
 * Purpose : Explicit no-lookahead release command for historical replay
 *
 * ClosedSlice:
 *   timestamp          = decision/bar timestamp T
 *   decision_timestamp = 0
 *
 * ExecutionOpen:
 *   timestamp          = executable open timestamp T+1
 *   decision_timestamp = originating decision timestamp T
 **************************************************************************************/
struct MarketDataReleaseRequest {
    ContractMetadata metadata;
    MarketDataReleaseKind kind = MarketDataReleaseKind::ClosedSlice;
    Timestamp timestamp = 0;
    Timestamp decision_timestamp = 0;
};
