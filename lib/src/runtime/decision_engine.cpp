#include "decision_engine.h"

#include <algorithm>
#include <utility>


DecisionEngine::DecisionEngine(
    StrategyPortfolio& strategies,
    const IndicatorEngine& indicators
)
    : strategies_(strategies),
      indicators_(indicators)
{}


// Update signals at a completed bar and create strategy decision intents
void DecisionEngine::onBarClose(
    const MarketData& signalData,
    const MarketData& sizingData,
    Timestamp ts,
    double accountEquity,
    const StrategyPositionSnapshot& executionPositions
)
{
    pending_decisions_.decision_timestamp = ts;

    for (auto& strategy : strategies_) {
        strategy.updateSignals(signalData, ts, indicators_);

        const double strategyCapital = accountEquity * strategy.allocationWeight();
        const auto plan = strategy.calculateRebalancePlan(
            sizingData,
            ts,
            strategyCapital,
            executionPositions.at(strategy.id())
        );

        if (!plan)
            continue;

        const auto existing = std::find_if(
            pending_decisions_.strategies.begin(),
            pending_decisions_.strategies.end(),
            [strategyId = strategy.id()](const StrategyDecisionIntent& value) {
                return value.strategy_id == strategyId;
            }
        );

        if (plan->size() == 0) {
            if (existing != pending_decisions_.strategies.end())
                pending_decisions_.strategies.erase(existing);
            continue;
        }

        StrategyDecisionIntent intent;
        intent.strategy_id = strategy.id();
        intent.decision_timestamp = plan->timestamp();
        intent.reference_capital = plan->referenceCapital();
        intent.decisions = plan->values();

        if (existing == pending_decisions_.strategies.end())
            pending_decisions_.strategies.push_back(std::move(intent));
        else
            *existing = std::move(intent);
    }

    last_bar_close_timestamp_ = ts;
}


void DecisionEngine::restoreState(
    const DecisionBatch& pendingDecisions,
    Timestamp lastBarCloseTimestamp
)
{
    pending_decisions_ = pendingDecisions;
    last_bar_close_timestamp_ = lastBarCloseTimestamp;
}
