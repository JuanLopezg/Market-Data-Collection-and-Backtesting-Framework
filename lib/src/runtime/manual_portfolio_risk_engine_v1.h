#pragma once

#include <cmath>
#include <set>
#include <stdexcept>
#include <string>

#include "decision_batch.h"
#include "execution_reference_prices.h"
#include "manual_target_intent_v1.h"
#include "mock_venue_catalog_v1.h"
#include "mock_venue_rules_v1.h"
#include "risk_constraints.h"
#include "strategy_position_snapshot.h"

namespace ManualControlV1 {

struct ManualRiskResultV1 {
    bool approved = false;
    std::string reason;
    DecisionBatch decisions;
};

class ManualPortfolioRiskEngineV1 {
public:
    explicit ManualPortfolioRiskEngineV1(
        double max_gross_leverage = 1.0,
        double max_asset_weight = 1.0)
        : constraints_(max_gross_leverage, max_asset_weight)
    {
    }

    ManualRiskResultV1 evaluate(
        const ManualTargetIntentV1& intent,
        double canonical_equity,
        const StrategyPositionSnapshot& strategy_positions,
        const ExecutionReferencePrices& closes) const
    {
        ManualRiskResultV1 result;
        if (!intent.valid()) {
            result.reason = "INVALID_MANUAL_TARGET_INTENT";
            return result;
        }
        if (!std::isfinite(canonical_equity) || canonical_equity <= 0.0) {
            result.reason = "INVALID_CANONICAL_EQUITY";
            return result;
        }

        TargetWeights requested;
        for (const auto& [asset, weight] : intent.asset_weights) {
            const auto* entry = MockVenueV1::findByCanonicalAssetExact(asset);
            const auto* rules = entry == nullptr ? nullptr : MockVenueV1::rulesForExact(*entry);
            if (entry == nullptr || !entry->enabled || rules == nullptr || !rules->valid()) {
                result.reason = "UNMAPPED_OR_INVALID_MOCK_ASSET:" + asset;
                return result;
            }
            if (!closes.contains(asset)) {
                result.reason = "MISSING_CLOSE_REFERENCE:" + asset;
                return result;
            }
            requested.set(asset, weight);
        }

        const TargetWeights approved = constraints_.apply(requested);
        for (const auto& [asset, requested_weight] : requested.values()) {
            if (std::abs(approved.get(asset) - requested_weight) > 1e-12) {
                result.reason = "RISK_CONSTRAINT_WOULD_MUTATE_REQUEST:" + asset;
                return result;
            }
        }

        const auto pos_it = strategy_positions.find(kManualStrategyId);
        const VirtualPositionState empty;
        const VirtualPositionState& current =
            pos_it == strategy_positions.end() ? empty : pos_it->second;

        std::set<Coin> assets;
        for (const auto& [asset, weight] : intent.asset_weights) {
            (void)weight;
            assets.insert(asset);
        }
        for (const auto& [asset, quantity] : current.values()) {
            (void)quantity;
            assets.insert(asset);
        }

        DecisionBatch batch;
        batch.decision_timestamp = intent.decision_timestamp;

        StrategyDecisionIntent decision;
        decision.strategy_id = kManualStrategyId;
        decision.decision_timestamp = intent.decision_timestamp;
        decision.reference_capital = canonical_equity;

        for (const Coin& asset : assets) {
            if (!closes.contains(asset)) {
                result.reason = "MISSING_CLOSE_REFERENCE:" + asset;
                return result;
            }
            const double target_weight = approved.get(asset);
            if (target_weight == 0.0) {
                if (current.get(asset) == 0.0)
                    continue;
                decision.decisions.emplace(asset, RebalanceDecision::flat());
                decision.target_notional_usd.emplace(asset, 0.0);
            } else {
                decision.decisions.emplace(
                    asset,
                    RebalanceDecision::targetWeight(target_weight));
                decision.target_notional_usd.emplace(
                    asset,
                    canonical_equity * target_weight);
            }
        }

        batch.strategies.push_back(std::move(decision));
        result.approved = true;
        result.reason = "APPROVED";
        result.decisions = std::move(batch);
        return result;
    }

private:
    RiskConstraints constraints_;
};

} // namespace ManualControlV1
