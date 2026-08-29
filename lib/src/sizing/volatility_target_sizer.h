#pragma once

#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "covariance_estimator.h"
#include "portfolio_sizer.h"
#include "portfolio_volatility_estimator.h"
#include "portfolio_weights.h"
#include "volatility_target.h"


/**************************************************************************************
 * Type    : VolatilityTargetSizer
 * Purpose : Produces strategy target weights by scaling signal shape to a vol target
 *
 * Signal values are used directly as the relative portfolio shape. No arbitrary 10%
 * base weight and no monetary capital are introduced before volatility targeting.
 *
 * Example:
 *   signals: BTC +1.0, ETH +0.5
 *   shape  : BTC +1.0, ETH +0.5
 *   -> covariance -> expected vol -> scaling -> TargetWeights
 **************************************************************************************/
class VolatilityTargetSizer final : public PortfolioSizer {
private:
    std::unique_ptr<CovarianceEstimator> covarianceEstimator_;
    PortfolioVolatilityEstimator volatilityEstimator_;
    VolatilityTarget volatilityTarget_;

public:
    VolatilityTargetSizer(
        std::unique_ptr<CovarianceEstimator> covarianceEstimator,
        double targetVolatility
    )
        : covarianceEstimator_(std::move(covarianceEstimator)),
          volatilityTarget_(targetVolatility)
    {
        if (!covarianceEstimator_)
            throw std::invalid_argument("Covariance estimator cannot be null");
    }

    PortfolioSizerKind kind() const override
    {
        return PortfolioSizerKind::VolatilityTarget;
    }

    std::optional<TargetWeights> size(
        const SignalState& signals,
        const MarketData& marketData,
        Timestamp timestamp
    ) const override
    {
        TargetWeights empty;
        if (signals.activeCount() == 0)
            return empty;

        PortfolioWeights signalShape;
        std::vector<Coin> coins;
        coins.reserve(signals.activeCount());

        for (const auto& [coin, signal] : signals.values()) {
            signalShape.set(coin, signal);
            coins.push_back(coin);
        }

        const auto covariance = covarianceEstimator_->estimate(marketData, timestamp, coins);
        if (!covariance)
            return std::nullopt;

        const double estimatedVolatility = volatilityEstimator_.estimate(signalShape, *covariance);
        const auto scaled = volatilityTarget_.apply(signalShape, estimatedVolatility);
        if (!scaled)
            return std::nullopt;

        return TargetWeights(*scaled);
    }

    std::optional<PortfolioSizingDiagnostics> diagnostics(
        const SignalState& signals,
        const TargetWeights& sizedWeights,
        const TargetWeights& constrainedWeights,
        const MarketData& marketData,
        Timestamp timestamp
    ) const override
    {
        if (signals.activeCount() == 0)
            return std::nullopt;

        PortfolioWeights signalShape;
        std::vector<Coin> coins;
        coins.reserve(signals.activeCount());

        for (const auto& [coin, signal] : signals.values()) {
            signalShape.set(coin, signal);
            coins.push_back(coin);
        }

        const auto covariance = covarianceEstimator_->estimate(marketData, timestamp, coins);
        if (!covariance)
            return std::nullopt;

        PortfolioSizingDiagnostics result;
        result.active_assets = signals.activeCount();
        result.target_volatility = volatilityTarget_.targetVolatility();
        result.raw_signal_volatility = volatilityEstimator_.estimate(signalShape, *covariance);
        result.scaling_factor = result.raw_signal_volatility > 0.0
            ? result.target_volatility / result.raw_signal_volatility
            : 0.0;
        result.pre_constraint_volatility = volatilityEstimator_.estimate(sizedWeights, *covariance);
        result.post_constraint_volatility = volatilityEstimator_.estimate(constrainedWeights, *covariance);
        return result;
    }
};
