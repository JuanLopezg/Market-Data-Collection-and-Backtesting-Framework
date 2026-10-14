#pragma once

#include <map>
#include <vector>

#include "decision_batch.h"
#include "portfolio_risk_engine.h"
#include "position_state.h"
#include "price_snapshot.h"

// Decisions that move strategy holdings to explicit weights. Shared by the manual
// rebalance (replay and trading chain) and by the re-sync after it.

// Per-strategy decisions that move holdings to `weights` of intent.reference_capital:
// TARGET for every positive weight, FLAT for held assets without one. Held assets with
// no price are left untouched (HOLD).
void setTargetDecisions(
    StrategyDecisionIntent& intent,
    const std::map<Coin, double>& weights,
    const VirtualPositionState* held,
    const ExecutionReferencePrices& prices);

// The first daily decision after a manual rebalance: every non-HOLD decision is kept and
// HOLD positions are moved to the risk-approved weight, so the account returns to the
// strategy targets. A strategy whose risk evaluation has no sizing keeps its normal
// decisions; a strategy that only had HOLDs is added from its evaluation.
DecisionBatch resyncToApprovedTargets(
    const DecisionBatch& approved,
    const std::vector<PortfolioRiskEvaluation>& evaluations,
    const StrategyPositionSnapshot& positions,
    const ExecutionReferencePrices& prices);
