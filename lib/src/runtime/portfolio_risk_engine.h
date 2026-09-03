#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "account_snapshot.h"
#include "decision_batch.h"
#include "portfolio_sizer.h"
#include "rebalance_policy.h"
#include "risk_constraints.h"
#include "strategy_intent_batch.h"


/**************************************************************************************
 * Type    : PortfolioRiskStrategyConfig
 * Purpose : Strategy-specific sizing/risk/rebalance policy owned outside Strategy service
 **************************************************************************************/
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


/**************************************************************************************
 * Type    : PortfolioRiskEngine
 * Purpose : Convert strategy signals into approved strategy target/rebalance intents
 *
 * This engine owns sizing, strategy allocation, portfolio risk constraints and rebalance
 * policy. It is deliberately blind to order creation, exchange protocols and fills.
 **************************************************************************************/
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
        const AccountSnapshot& account
    );

    Timestamp lastTimestamp() const { return last_timestamp_; }
};
