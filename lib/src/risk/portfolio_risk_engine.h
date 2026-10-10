#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "account_snapshot.h"
#include "decision_batch.h"
#include "portfolio_sizer.h"
#include "rebalance_policy.h"
#include "risk_constraints.h"
#include "strategy_intent_batch.h"


// Strategy-specific sizing/risk/rebalance policy owned outside Strategy service
struct PortfolioRiskStrategyConfig {
    StrategyID strategy_id = 0;
    std::string strategy_name;
    double allocation_weight = 0.0;
    std::unique_ptr<PortfolioSizer> portfolio_sizer;
    RiskConstraints risk_constraints;
    std::unique_ptr<RebalancePolicy> rebalance_policy;

    PortfolioRiskStrategyConfig(
        StrategyID strategyId,
        std::string strategyName,
        double allocationWeight,
        std::unique_ptr<PortfolioSizer> portfolioSizer,
        RiskConstraints riskConstraints,
        std::unique_ptr<RebalancePolicy> rebalancePolicy
    );

    PortfolioRiskStrategyConfig(PortfolioRiskStrategyConfig&&) noexcept = default;
    PortfolioRiskStrategyConfig& operator=(PortfolioRiskStrategyConfig&&) noexcept = default;
    PortfolioRiskStrategyConfig(const PortfolioRiskStrategyConfig&) = delete;
    PortfolioRiskStrategyConfig& operator=(const PortfolioRiskStrategyConfig&) = delete;
};


// Optional observation of one strategy evaluation, separate from executable intent.
struct PortfolioRiskEvaluation {
    StrategyID strategy_id = 0;
    std::string strategy_name;
    PortfolioSizerKind sizer_kind = PortfolioSizerKind::Generic;
    double reference_capital = 0.0;
    double max_gross_leverage = 0.0;
    double max_asset_weight = 0.0;
    bool sizing_available = false;
    TargetWeights sized_weights;
    TargetWeights approved_weights;
    RiskConstraintEvaluation constraints;
    std::optional<PortfolioSizingDiagnostics> volatility;
    std::string volatility_state = "NOT_APPLICABLE";
    std::unordered_map<Coin, RebalanceDecision> rebalance_decisions;
    std::unordered_map<Coin, double> current_quantities;
};

// Convert strategy signals into approved strategy target/rebalance intents.
// This engine owns sizing, strategy allocation, portfolio risk constraints and rebalance
// policy. It is deliberately blind to order creation, exchange protocols and fills.
class PortfolioRiskEngine {
private:
    std::vector<PortfolioRiskStrategyConfig> strategies_;
    Timestamp last_timestamp_ = 0;

    static double accountEquity(
        const AccountSnapshot& account,
        const MarketData& marketData,
        Timestamp timestamp
    );

public:
    explicit PortfolioRiskEngine(std::vector<PortfolioRiskStrategyConfig> strategies);

    DecisionBatch onSignals(
        const StrategyIntentBatch& signals,
        const MarketData& marketData,
        const AccountSnapshot& account,
        std::vector<PortfolioRiskEvaluation>* evaluations = nullptr
    );

    Timestamp lastTimestamp() const { return last_timestamp_; }
    void restoreLastTimestamp(Timestamp timestamp);
};
