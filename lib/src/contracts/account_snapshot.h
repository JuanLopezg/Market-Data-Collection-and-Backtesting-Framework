#pragma once

#include <unordered_map>

#include "contract_metadata.h"


// Read-only business-state projection owned by ExecutionEngine
struct AccountSnapshot {
    ContractMetadata metadata;
    Timestamp timestamp = 0;
    double cash = 0.0;
    // Venue collateral (BTC on Kraken Multi-M) in USD after the venue haircut. Counts in the
    // equity portfolio risk sizes with (user decision 2026-10-14); orders never spend it.
    double collateral_usd = 0.0;
    std::unordered_map<Coin, double> positions;
    std::unordered_map<StrategyID, std::unordered_map<Coin, double>> strategy_positions;
    // Set on the first daily snapshot after a manual rebalance: portfolio risk then moves
    // every strategy back to its approved target weights, HOLD positions included.
    bool resync_to_targets = false;
};
