#pragma once

#include <unordered_map>

#include "contract_metadata.h"
#include "data_types.h"


/**************************************************************************************
 * Type    : DailyCloseSnapshot
 * Purpose : Immutable close(T) reference prices read from canonical SQLite
 *
 * These are NOT execution/fill prices. They are the completed Binance daily closes used
 * as the common valuation reference for the close-T decision/planning cycle.
 **************************************************************************************/
struct DailyCloseSnapshot {
    ContractMetadata metadata;
    Timestamp date = 0;
    std::unordered_map<Coin, double> closes;
};
