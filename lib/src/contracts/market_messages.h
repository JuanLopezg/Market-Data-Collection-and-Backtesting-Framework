#pragma once

// Market readiness, complete slices and close-time valuation messages.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "contract_metadata.h"
#include "data_types.h"

// Replay-control phase released by the historical market-data service
enum class MarketDataReleaseKind : int {
    ClosedSlice = 1,
    ExecutionOpen = 2
};


// Explicit no-lookahead release command for historical replay
//
// ClosedSlice:
// timestamp          = decision/bar timestamp T
// decision_timestamp = 0
//
// ExecutionOpen:
// timestamp          = executable open timestamp T+1
// decision_timestamp = originating decision timestamp T
struct MarketDataReleaseRequest {
    ContractMetadata metadata;
    MarketDataReleaseKind kind = MarketDataReleaseKind::ClosedSlice;
    Timestamp timestamp = 0;
    Timestamp decision_timestamp = 0;
};

// Durable notification that the canonical market-data SQLite database has
// been successfully updated through one completed daily trading date.
//
// This is intentionally a lightweight readiness/event contract. It does not carry the
// OHLCV payload itself. Downstream services that choose to react to it will load the
// history they require from the canonical market-data database.
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

// Barrier event emitted only after one complete cross-sectional market slice
//
// DecisionEngine must consume this event as a slice boundary rather than acting on the
// first individual symbol bar. This preserves cross-sectional universes/rankers.
struct MarketSliceClosed {
    ContractMetadata metadata;
    Timestamp timestamp = 0;
    std::vector<Coin> symbols;
};

// One completed OHLCV bar inside a cross-sectional market slice
struct MarketBarSnapshot {
    Coin coin;
    OHLCV bar;
};


// Complete cross-sectional market-data barrier consumed by Decision runtime
//
// A message represents one fully closed timestamp. Decision may append it to its local
// rolling history only as one atomic slice; it must never act on the first individual
// symbol received for that timestamp.
struct MarketSliceSnapshot {
    ContractMetadata metadata;
    Timestamp timestamp = 0;
    std::vector<MarketBarSnapshot> bars;
};

// Immutable close(T) reference prices read from canonical SQLite
//
// These are NOT execution/fill prices. They are the completed Binance daily closes used
// as the common valuation reference for the close-T decision/planning cycle.
struct DailyCloseSnapshot {
    ContractMetadata metadata;
    Timestamp date = 0;
    std::unordered_map<Coin, double> closes;
};
