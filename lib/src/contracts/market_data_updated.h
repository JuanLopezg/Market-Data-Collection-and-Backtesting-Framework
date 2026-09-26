#pragma once

#include <cstdint>
#include <string>

#include "contract_metadata.h"


/**************************************************************************************
 * Type    : MarketDataUpdated
 * Purpose : Durable notification that the canonical market-data SQLite database has
 *           been successfully updated through one completed daily trading date.
 *
 * This is intentionally a lightweight readiness/event contract. It does not carry the
 * OHLCV payload itself. Downstream services that choose to react to it will load the
 * history they require from the canonical market-data database.
 **************************************************************************************/
struct MarketDataUpdated {
    ContractMetadata metadata;

    // Compact YYYYMMDD date of the newest completed daily candle covered by the update.
    Timestamp completed_through = 0;

    std::string source = "binance";
    std::string timeframe = "1d";

    std::uint64_t ranked_symbols = 0;
    std::uint64_t active_top_n = 0;
    std::uint64_t tracked_symbols = 0;
    std::uint64_t maintained_symbols = 0;
    std::uint64_t requested_symbols = 0;
    std::uint64_t downloaded_rows = 0;
};
