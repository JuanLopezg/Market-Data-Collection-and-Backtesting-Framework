#pragma once

#include "decision_batch.h"
#include "indicator_engine.h"
#include "strategy_instance.h"
#include "strategy_position_snapshot.h"


/**************************************************************************************
 * Type    : DecisionEngine
 * Purpose : Own strategy-side decision state and produce transport-independent intent
 *
 * Decision reads execution-owned filled positions but cannot mutate them. Its output is a
 * DecisionBatch containing weights/reference capital, never executable quantities.
 **************************************************************************************/
class DecisionEngine {
private:
    StrategyPortfolio& strategies_;
    const IndicatorEngine& indicators_;

    DecisionBatch pending_decisions_;
    Timestamp last_bar_close_timestamp_ = 0;

public:
    DecisionEngine(
        StrategyPortfolio& strategies,
        const IndicatorEngine& indicators
    );

    void onBarClose(
        const MarketData& marketData,
        Timestamp ts,
        double accountEquity,
        const StrategyPositionSnapshot& executionPositions
    );

    bool hasPendingDecisions() const { return !pending_decisions_.strategies.empty(); }

    const DecisionBatch& pendingDecisions() const
    {
        return pending_decisions_;
    }

    void clearPendingDecisions() { pending_decisions_ = DecisionBatch{}; }

    Timestamp lastBarCloseTimestamp() const { return last_bar_close_timestamp_; }

    void restoreState(
        const DecisionBatch& pendingDecisions,
        Timestamp lastBarCloseTimestamp
    );
};
