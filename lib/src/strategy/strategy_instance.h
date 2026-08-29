#pragma once

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "fill.h"
#include "portfolio_sizer.h"
#include "rebalance_plan.h"
#include "rebalance_policy.h"
#include "risk_constraints.h"
#include "signal_state.h"
#include "strategy.h"
#include "target_weights.h"
#include "virtual_position_state.h"


class IndicatorEngine;


/**************************************************************************************
 * Type    : StrategyPortfolio
 * Purpose : Collection of independently configured strategy instances
 **************************************************************************************/
class StrategyInstance;
using StrategyPortfolio = std::vector<StrategyInstance>;


/**************************************************************************************
 * Type    : StrategyInstance
 * Purpose : Own one strategy plus its sizing/risk/rebalance configuration and attribution
 *
 * The instance is independent from Account/Backtester. Strategy capital is supplied when
 * a rebalance plan is calculated, which keeps this object reusable in backtest and live.
 **************************************************************************************/
class StrategyInstance {
private:
    StrategyID strategy_id_ = 0;

    std::unique_ptr<Strategy> strategy_;
    std::unique_ptr<PortfolioSizer> portfolio_sizer_;
    std::unique_ptr<RebalancePolicy> rebalance_policy_;

    RiskConstraints risk_constraints_;
    double allocation_weight_ = 0.0;

    SignalState signal_state_;
    TargetWeights desired_weights_;
    VirtualPositionState virtual_positions_;
    std::optional<PortfolioSizingDiagnostics> last_sizing_diagnostics_;

public:
    StrategyInstance(
        StrategyID strategyId,
        std::unique_ptr<Strategy> strategy,
        double allocationWeight,
        std::unique_ptr<PortfolioSizer> portfolioSizer,
        RiskConstraints riskConstraints,
        std::unique_ptr<RebalancePolicy> rebalancePolicy
    )
        : strategy_id_(strategyId),
          strategy_(std::move(strategy)),
          portfolio_sizer_(std::move(portfolioSizer)),
          rebalance_policy_(std::move(rebalancePolicy)),
          risk_constraints_(std::move(riskConstraints)),
          allocation_weight_(allocationWeight)
    {
        if (!strategy_)
            throw std::invalid_argument("Strategy cannot be null");
        if (!portfolio_sizer_)
            throw std::invalid_argument("Portfolio sizer cannot be null");
        if (!rebalance_policy_)
            throw std::invalid_argument("Rebalance policy cannot be null");
        if (!std::isfinite(allocation_weight_) || allocation_weight_ < 0.0)
            throw std::invalid_argument("Strategy allocation weight must be finite and non-negative");
    }

    void updateSignals(
        const MarketData& marketData,
        Timestamp ts,
        const IndicatorEngine& indicators
    )
    {
        strategy_->updateSignals(marketData, ts, signal_state_, indicators);
    }

    /**************************************************************************************
     * Purpose : Calculate desired weights and strategy-level rebalance decisions
     *
     * strategyCapital is supplied by Account/portfolio orchestration. It is captured in
     * RebalancePlan so a target weight can later be converted at the actual execution time.
     **************************************************************************************/
    std::optional<RebalancePlan> calculateRebalancePlan(
        const MarketData& marketData,
        Timestamp ts,
        double strategyCapital
    )
    {
        if (!std::isfinite(strategyCapital) || strategyCapital < 0.0)
            throw std::invalid_argument("Strategy capital must be finite and non-negative");

        const auto sizedWeights = portfolio_sizer_->size(signal_state_, marketData, ts);
        if (!sizedWeights) {
            last_sizing_diagnostics_.reset();
            return std::nullopt;
        }

        desired_weights_ = risk_constraints_.apply(*sizedWeights);
        last_sizing_diagnostics_ = portfolio_sizer_->diagnostics(
            signal_state_,
            *sizedWeights,
            desired_weights_,
            marketData,
            ts
        );

        if (last_sizing_diagnostics_) {
            auto& diagnostics = *last_sizing_diagnostics_;
            diagnostics.max_gross_leverage_limit = risk_constraints_.maxGrossLeverage();
            diagnostics.max_asset_weight_limit = risk_constraints_.maxAssetWeight();

            double grossAfterAssetCap = 0.0;
            for (const auto& [coin, weight] : sizedWeights->values()) {
                (void)coin;
                diagnostics.gross_before_constraints += std::abs(weight);
                if (std::abs(weight) > diagnostics.max_asset_weight_limit + 1e-12)
                    diagnostics.asset_cap_binding = true;

                grossAfterAssetCap += std::min(
                    std::abs(weight),
                    diagnostics.max_asset_weight_limit
                );
            }

            for (const auto& [coin, weight] : desired_weights_.values()) {
                (void)coin;
                diagnostics.gross_after_constraints += std::abs(weight);
                diagnostics.max_asset_weight_after_constraints = std::max(
                    diagnostics.max_asset_weight_after_constraints,
                    std::abs(weight)
                );
            }

            diagnostics.gross_cap_binding =
                grossAfterAssetCap > diagnostics.max_gross_leverage_limit + 1e-12;
        }

        RebalancePlan plan(ts, strategyCapital);

        std::unordered_set<Coin> coins;
        for (const auto& [coin, signal] : signal_state_.values()) {
            (void)signal;
            coins.insert(coin);
        }
        for (const auto& [coin, weight] : desired_weights_.values()) {
            (void)weight;
            coins.insert(coin);
        }
        for (const auto& [coin, quantity] : virtual_positions_.values()) {
            (void)quantity;
            coins.insert(coin);
        }

        const auto tsIt = marketData.find(ts);

        for (const Coin& coin : coins) {
            const double signal = signal_state_.get(coin);
            const double desiredWeight = desired_weights_.get(coin);
            const double currentQuantity = virtual_positions_.get(coin);

            double currentPrice = 0.0;
            if (tsIt != marketData.end()) {
                const auto barIt = tsIt->second.find(coin);
                if (barIt != tsIt->second.end())
                    currentPrice = barIt->second.close;
            }

            const RebalanceDecision decision = rebalance_policy_->decide(
                signal,
                desiredWeight,
                currentQuantity,
                currentPrice,
                strategyCapital
            );

            plan.set(coin, decision);
            if (last_sizing_diagnostics_ && decision.action != RebalanceAction::Hold)
                ++last_sizing_diagnostics_->rebalance_actions;
        }

        return plan;
    }

    const std::optional<PortfolioSizingDiagnostics>& sizingDiagnostics() const
    {
        return last_sizing_diagnostics_;
    }

    void applyVirtualFill(const Fill& fill)
    {
        if (fill.strategy_id != strategy_id_)
            throw std::invalid_argument("Fill strategy id does not match StrategyInstance");

        virtual_positions_.add(fill.coin, fill.signedQuantity());
    }

    /**************************************************************************************
     * Purpose : Restore persistent strategy-owned operational state after restart
     **************************************************************************************/
    void restoreState(
        const std::unordered_map<Coin, double>& signals,
        const std::unordered_map<Coin, double>& desiredWeights,
        const std::unordered_map<Coin, double>& virtualPositions
    )
    {
        SignalState restoredSignals;
        for (const auto& [coin, signal] : signals)
            restoredSignals.set(coin, signal);

        TargetWeights restoredWeights;
        for (const auto& [coin, weight] : desiredWeights)
            restoredWeights.set(coin, weight);

        VirtualPositionState restoredPositions;
        for (const auto& [coin, quantity] : virtualPositions)
            restoredPositions.set(coin, quantity);

        signal_state_ = std::move(restoredSignals);
        desired_weights_ = std::move(restoredWeights);
        virtual_positions_ = std::move(restoredPositions);
        last_sizing_diagnostics_.reset();
    }

    StrategyID id() const
    {
        return strategy_id_;
    }

    const std::string& name() const
    {
        return strategy_->name();
    }

    double allocationWeight() const
    {
        return allocation_weight_;
    }

    PortfolioSizerKind portfolioSizerKind() const
    {
        return portfolio_sizer_->kind();
    }

    SignalState& signalState()
    {
        return signal_state_;
    }

    const SignalState& signalState() const
    {
        return signal_state_;
    }

    const TargetWeights& desiredWeights() const
    {
        return desired_weights_;
    }

    VirtualPositionState& virtualPositions()
    {
        return virtual_positions_;
    }

    const VirtualPositionState& virtualPositions() const
    {
        return virtual_positions_;
    }

    Strategy& strategy()
    {
        return *strategy_;
    }

    const Strategy& strategy() const
    {
        return *strategy_;
    }

    // Compatibility names retained while callers migrate to the cleaner accessors above.
    SignalState& GetSignalState() { return signalState(); }
    const SignalState& GetSignalState() const { return signalState(); }
    const TargetWeights& GetDesiredWeights() const { return desiredWeights(); }
    VirtualPositionState& GetVirtualPositions() { return virtualPositions(); }
    const VirtualPositionState& GetVirtualPositions() const { return virtualPositions(); }
    double& GetWeight() { return allocation_weight_; }
    const double& GetWeight() const { return allocation_weight_; }
    Strategy& GetStrategy() { return strategy(); }
    const Strategy& GetStrategy() const { return strategy(); }
};
