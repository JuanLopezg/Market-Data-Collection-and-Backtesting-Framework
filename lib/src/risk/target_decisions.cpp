#include "target_decisions.h"

#include <algorithm>


void setTargetDecisions(
    StrategyDecisionIntent& intent,
    const std::map<Coin, double>& weights,
    const VirtualPositionState* held,
    const ExecutionReferencePrices& prices)
{
    intent.decisions.clear();
    intent.target_notional_usd.clear();
    std::map<Coin, double> assets = weights;
    if (held != nullptr)
        for (const auto& [asset, quantity] : held->values())
            if (quantity != 0.0)
                assets.emplace(asset, 0.0);

    for (const auto& [asset, weight] : assets) {
        if (!prices.contains(asset))
            continue;
        if (weight > 0.0) {
            intent.decisions.emplace(asset, RebalanceDecision::targetWeight(weight));
            intent.target_notional_usd.emplace(asset, intent.reference_capital * weight);
        } else {
            intent.decisions.emplace(asset, RebalanceDecision::flat());
            intent.target_notional_usd.emplace(asset, 0.0);
        }
    }
}


DecisionBatch resyncToApprovedTargets(
    const DecisionBatch& approved,
    const std::vector<PortfolioRiskEvaluation>& evaluations,
    const StrategyPositionSnapshot& positions,
    const ExecutionReferencePrices& prices)
{
    const auto approvedWeights = [](const PortfolioRiskEvaluation& evaluation) {
        std::map<Coin, double> weights;
        for (const auto& [asset, weight] : evaluation.approved_weights.values())
            if (weight > 0.0)
                weights.emplace(asset, weight);
        return weights;
    };
    const auto heldBy = [&](StrategyID strategyId) -> const VirtualPositionState* {
        const auto it = positions.find(strategyId);
        return it == positions.end() ? nullptr : &it->second;
    };

    DecisionBatch resync = approved;
    for (StrategyDecisionIntent& intent : resync.strategies) {
        const auto evaluation = std::find_if(evaluations.begin(), evaluations.end(),
            [&](const PortfolioRiskEvaluation& value) { return value.strategy_id == intent.strategy_id; });
        if (evaluation == evaluations.end() || !evaluation->sizing_available)
            continue;
        StrategyDecisionIntent moved = intent;
        setTargetDecisions(moved, approvedWeights(*evaluation), heldBy(intent.strategy_id), prices);
        // Only HOLDs move to the approved weight: the day's explicit entries and exits stand.
        for (const auto& [asset, decision] : intent.decisions) {
            if (decision.action == RebalanceAction::Hold)
                continue;
            moved.decisions.insert_or_assign(asset, decision);
            const auto notional = intent.target_notional_usd.find(asset);
            if (notional != intent.target_notional_usd.end())
                moved.target_notional_usd.insert_or_assign(asset, notional->second);
            else
                moved.target_notional_usd.erase(asset);
        }
        intent = std::move(moved);
    }

    // Risk omits a strategy whose decisions were all HOLD; after a manual rebalance its
    // holdings still have to move back.
    for (const PortfolioRiskEvaluation& evaluation : evaluations) {
        if (!evaluation.sizing_available)
            continue;
        const bool present = std::any_of(resync.strategies.begin(), resync.strategies.end(),
            [&](const StrategyDecisionIntent& intent) { return intent.strategy_id == evaluation.strategy_id; });
        if (present)
            continue;
        StrategyDecisionIntent intent;
        intent.strategy_id = evaluation.strategy_id;
        intent.decision_timestamp = approved.decision_timestamp;
        intent.reference_capital = evaluation.reference_capital;
        setTargetDecisions(intent, approvedWeights(evaluation), heldBy(evaluation.strategy_id), prices);
        if (!intent.decisions.empty())
            resync.strategies.push_back(std::move(intent));
    }
    return resync;
}
