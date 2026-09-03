#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "indicator_spec.h"
#include "signal_state.h"
#include "strategy.h"


class IndicatorEngine;


/**************************************************************************************
 * Type    : StrategySignalInstance
 * Purpose : Signal-only runtime ownership for one Strategy
 *
 * Unlike StrategyInstance, this object intentionally owns no PortfolioSizer,
 * RiskConstraints, RebalancePolicy or virtual positions. It is therefore suitable for
 * the standalone strategy-service boundary.
 **************************************************************************************/
class StrategySignalInstance {
private:
    StrategyID strategy_id_ = 0;
    std::unique_ptr<Strategy> strategy_;
    SignalState signal_state_;

public:
    StrategySignalInstance(
        StrategyID strategyId,
        std::unique_ptr<Strategy> strategy
    )
        : strategy_id_(strategyId),
          strategy_(std::move(strategy))
    {
        if (!strategy_)
            throw std::invalid_argument("Strategy cannot be null");
    }

    void updateSignals(
        const MarketData& marketData,
        Timestamp ts,
        const IndicatorEngine& indicators
    )
    {
        strategy_->updateSignals(marketData, ts, signal_state_, indicators);
    }

    void restoreSignals(const std::unordered_map<Coin, double>& signals)
    {
        SignalState restored;
        for (const auto& [coin, signal] : signals)
            restored.set(coin, signal);
        signal_state_ = std::move(restored);
    }

    StrategyID id() const { return strategy_id_; }
    const std::string& name() const { return strategy_->name(); }
    const SignalState& signalState() const { return signal_state_; }
    SignalState& signalState() { return signal_state_; }

    const Strategy& strategy() const { return *strategy_; }
    Strategy& strategy() { return *strategy_; }

    std::vector<IndicatorSpec> requiredIndicators() const
    {
        return strategy_->requiredIndicators();
    }
};

using StrategySignalPortfolio = std::vector<StrategySignalInstance>;
