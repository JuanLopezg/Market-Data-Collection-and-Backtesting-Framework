#pragma once

#include "account_snapshot.h"
#include "equal_weight_sizer.h"
#include "entry_exit_only_rebalance_policy.h"
#include "portfolio_risk_engine.h"
#include "sample_covariance_estimator.h"
#include "volatility_target_sizer.h"

#include <initializer_list>
#include <map>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace risk_test {

constexpr Timestamp date = 20240103;
using Opinions = std::unordered_map<Coin, double>;

inline MarketData market(const std::map<Coin, std::vector<double>>& closes)
{
    OHLCVData raw;
    for (const auto& [coin, series] : closes)
        for (std::size_t i = 0; i < series.size(); ++i) {
            const double close = series[i];
            raw.data[coin][20240101u + static_cast<Timestamp>(i)] =
                {close, close * 1.1, close * 0.9, close, 1};
        }
    return buildMarketData(raw);
}

inline MarketData marks()
{
    return market({{"A", {100, 100, 100}}, {"B", {50, 50, 50}}, {"C", {20, 20, 20}}});
}

inline MarketData volatilityHistory()
{
    return market({{"A", {100, 125, 93.75}}, {"B", {100, 50, 75}}});
}

inline TargetWeights weights(std::initializer_list<std::pair<Coin, double>> values)
{
    TargetWeights result;
    for (const auto& [coin, value] : values) result.set(coin, value);
    return result;
}

inline SignalState signalState(const Opinions& values)
{
    SignalState result;
    for (const auto& [coin, value] : values) result.set(coin, value);
    return result;
}

inline std::unique_ptr<VolatilityTargetSizer> volSizer(double target = 0.25, double periods = 2)
{
    return std::make_unique<VolatilityTargetSizer>(
        std::make_unique<SampleCovarianceEstimator>(2, periods), target);
}

struct RiskSettings {
    StrategyID id = 7;
    double allocation = 1;
    double full_weight = 0.25;
    double gross_cap = 1;
    double asset_cap = 1;
};

inline PortfolioRiskStrategyConfig makeConfig(RiskSettings settings = {},
    std::unique_ptr<PortfolioSizer> sizer = {}, std::unique_ptr<RebalancePolicy> policy = {})
{
    if (!sizer) sizer = std::make_unique<EqualWeightSizer>(settings.full_weight);
    if (!policy) policy = std::make_unique<EntryExitOnlyRebalancePolicy>();
    return {settings.id, "Pure_RSI", settings.allocation, std::move(sizer),
        RiskConstraints(settings.gross_cap, settings.asset_cap), std::move(policy)};
}

inline PortfolioRiskEngine makeEngine(RiskSettings settings = {},
    std::unique_ptr<PortfolioSizer> sizer = {}, std::unique_ptr<RebalancePolicy> policy = {})
{
    std::vector<PortfolioRiskStrategyConfig> configs;
    configs.push_back(makeConfig(settings, std::move(sizer), std::move(policy)));
    return PortfolioRiskEngine(std::move(configs));
}

inline AccountSnapshot account(double cash = 1000, Timestamp timestamp = date)
{
    AccountSnapshot result;
    result.timestamp = timestamp;
    result.cash = cash;
    result.strategy_positions.emplace(7, Opinions{});
    return result;
}

inline StrategyIntentBatch signals(Opinions opinions = {{"A", 1}}, Timestamp timestamp = date)
{
    StrategyIntentBatch result;
    result.timestamp = timestamp;
    result.strategies.push_back({7, "Pure_RSI", std::move(opinions)});
    return result;
}

inline const StrategyDecisionIntent& decision(const DecisionBatch& batch, StrategyID id = 7)
{
    for (const auto& intent : batch.strategies)
        if (intent.strategy_id == id) return intent;
    throw std::out_of_range("Expected strategy decision is missing");
}

inline StrategyDecisionIntent targetIntent(double capital = 1000)
{
    StrategyDecisionIntent result;
    result.strategy_id = 7;
    result.decision_timestamp = date;
    result.reference_capital = capital;
    return result;
}

} // namespace risk_test
