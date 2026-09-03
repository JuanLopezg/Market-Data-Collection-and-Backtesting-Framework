#pragma once

#include <vector>

#include "indicator_engine.h"
#include "strategy_intent_batch.h"
#include "strategy_signal_instance.h"


/**************************************************************************************
 * Type    : StrategySignalEngine
 * Purpose : Run strategies only and emit signal snapshots
 *
 * This engine is deliberately blind to account equity, sizing, risk, rebalance policy,
 * positions and orders. It answers only: "what does each strategy currently signal?"
 **************************************************************************************/
class StrategySignalEngine {
private:
    StrategySignalPortfolio strategies_;
    IndicatorEngine indicators_;
    std::vector<IndicatorSpec> required_indicators_;
    Timestamp last_timestamp_ = 0;

public:
    explicit StrategySignalEngine(StrategySignalPortfolio strategies);

    StrategyIntentBatch onBarClose(
        const OHLCVData& rawData,
        const MarketData& marketData,
        Timestamp ts
    );

    Timestamp lastTimestamp() const { return last_timestamp_; }

    const StrategySignalPortfolio& strategies() const { return strategies_; }
    StrategySignalPortfolio& strategies() { return strategies_; }
};
