#include "portfolio_risk_engine.h"

#include <cmath>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "rebalance_plan.h"
#include "signal_state.h"
#include "virtual_position_state.h"


PortfolioRiskStrategyConfig::PortfolioRiskStrategyConfig(
    StrategyID strategyId,
    std::string strategyName,
    double allocationWeight,
    std::unique_ptr<PortfolioSizer> portfolioSizer,
    RiskConstraints riskConstraints,
    std::unique_ptr<RebalancePolicy> rebalancePolicy
)
    : strategy_id(strategyId),
      strategy_name(std::move(strategyName)),
      allocation_weight(allocationWeight),
      portfolio_sizer(std::move(portfolioSizer)),
      risk_constraints(std::move(riskConstraints)),
      rebalance_policy(std::move(rebalancePolicy))
{
    if (strategy_id == 0)
        throw std::invalid_argument("Portfolio-risk strategy id must be non-zero");
    if (strategy_name.empty())
        throw std::invalid_argument("Portfolio-risk strategy name cannot be empty");
    if (!std::isfinite(allocation_weight) || allocation_weight < 0.0)
        throw std::invalid_argument("Strategy allocation weight must be finite and non-negative");
    if (!portfolio_sizer)
        throw std::invalid_argument("Portfolio-risk strategy requires a portfolio sizer");
    if (!rebalance_policy)
        throw std::invalid_argument("Portfolio-risk strategy requires a rebalance policy");
}


PortfolioRiskEngine::PortfolioRiskEngine(std::vector<PortfolioRiskStrategyConfig> strategies)
    : strategies_(std::move(strategies))
{
    if (strategies_.empty())
        throw std::invalid_argument("PortfolioRiskEngine requires at least one strategy");

    std::unordered_set<StrategyID> ids;
    for (const auto& strategy : strategies_) {
        if (!ids.insert(strategy.strategy_id).second)
            throw std::invalid_argument("PortfolioRiskEngine strategy ids must be unique");
    }
}


double PortfolioRiskEngine::accountEquity(
    const AccountSnapshot& account,
    const MarketData& marketData,
    Timestamp timestamp
)
{
    if (!std::isfinite(account.cash))
        throw std::invalid_argument("Account snapshot cash must be finite");

    const auto tsIt = marketData.find(timestamp);
    if (tsIt == marketData.end())
        throw std::out_of_range("Portfolio-risk timestamp is missing from market data");

    double equity = account.cash;
    for (const auto& [coin, quantity] : account.positions) {
        if (!std::isfinite(quantity))
            throw std::invalid_argument("Account snapshot position must be finite");

        const auto barIt = tsIt->second.find(coin);
        if (barIt == tsIt->second.end())
            throw std::runtime_error("Missing close price for account position");
        if (!std::isfinite(barIt->second.close) || barIt->second.close <= 0.0)
            throw std::runtime_error("Invalid close price for account position");

        equity += quantity * barIt->second.close;
    }

    if (!std::isfinite(equity) || equity <= 0.0)
        throw std::runtime_error("Account equity must be finite and positive");
    return equity;
}


DecisionBatch PortfolioRiskEngine::onSignals(
    const StrategyIntentBatch& signals,
    const MarketData& marketData,
    const AccountSnapshot& account
)
{
    const Timestamp timestamp = signals.timestamp;
    if (timestamp == 0)
        throw std::invalid_argument("Portfolio-risk signal timestamp must be non-zero");
    if (last_timestamp_ != 0 && timestamp <= last_timestamp_)
        throw std::logic_error("Portfolio-risk timestamps must be strictly increasing");
    if (account.timestamp != timestamp)
        throw std::invalid_argument("Portfolio-risk requires account state for the same timestamp");

    std::unordered_map<StrategyID, const StrategySignalIntent*> signalByStrategy;
    signalByStrategy.reserve(signals.strategies.size());
    for (const StrategySignalIntent& intent : signals.strategies) {
        if (!signalByStrategy.emplace(intent.strategy_id, &intent).second)
            throw std::invalid_argument("Strategy signal batch contains duplicate strategy id");
    }

    if (signalByStrategy.size() != strategies_.size())
        throw std::invalid_argument("Strategy signal batch does not match configured strategies");

    const double equity = accountEquity(account, marketData, timestamp);

    DecisionBatch output;
    output.decision_timestamp = timestamp;
    output.strategies.reserve(strategies_.size());

    for (const auto& strategy : strategies_) {
        const auto signalIt = signalByStrategy.find(strategy.strategy_id);
        if (signalIt == signalByStrategy.end())
            throw std::invalid_argument("Strategy signal batch is missing configured strategy");
        if (signalIt->second->strategy_name != strategy.strategy_name)
            throw std::invalid_argument("Strategy signal name does not match portfolio-risk config");

        SignalState signalState;
        for (const auto& [coin, signal] : signalIt->second->signals) {
            if (coin.empty())
                throw std::invalid_argument("Strategy signal contains empty coin");
            signalState.set(coin, signal);
        }

        const double strategyCapital = equity * strategy.allocation_weight;
        const auto sizedWeights = strategy.portfolio_sizer->size(
            signalState,
            marketData,
            timestamp
        );
        if (!sizedWeights)
            continue;

        const TargetWeights desiredWeights = strategy.risk_constraints.apply(*sizedWeights);

        VirtualPositionState currentPositions;
        const auto positionsIt = account.strategy_positions.find(strategy.strategy_id);
        if (positionsIt == account.strategy_positions.end())
            throw std::invalid_argument("Account snapshot is missing configured strategy positions");

        for (const auto& [coin, quantity] : positionsIt->second) {
            if (!std::isfinite(quantity))
                throw std::invalid_argument("Strategy position must be finite");
            currentPositions.set(coin, quantity);
        }

        std::unordered_set<Coin> coins;
        for (const auto& [coin, signal] : signalState.values()) {
            (void)signal;
            coins.insert(coin);
        }
        for (const auto& [coin, weight] : desiredWeights.values()) {
            (void)weight;
            coins.insert(coin);
        }
        for (const auto& [coin, quantity] : currentPositions.values()) {
            (void)quantity;
            coins.insert(coin);
        }

        StrategyDecisionIntent decisionIntent;
        decisionIntent.strategy_id = strategy.strategy_id;
        decisionIntent.decision_timestamp = timestamp;
        decisionIntent.reference_capital = strategyCapital;

        const auto tsIt = marketData.find(timestamp);
        for (const Coin& coin : coins) {
            const double signal = signalState.get(coin);
            const double desiredWeight = desiredWeights.get(coin);
            const double currentQuantity = currentPositions.get(coin);

            double currentPrice = 0.0;
            if (tsIt != marketData.end()) {
                const auto barIt = tsIt->second.find(coin);
                if (barIt != tsIt->second.end())
                    currentPrice = barIt->second.close;
            }

            const RebalanceDecision decision = strategy.rebalance_policy->decide(
                signal,
                desiredWeight,
                currentQuantity,
                currentPrice,
                strategyCapital
            );

            // RebalancePlan semantics define a missing coin as HOLD. Keep the distributed
            // DecisionBatch boundary identical: explicit HOLD entries must not be emitted.
            if (decision.action != RebalanceAction::Hold)
                decisionIntent.decisions.emplace(coin, decision);
        }

        if (!decisionIntent.decisions.empty())
            output.strategies.push_back(std::move(decisionIntent));
    }

    last_timestamp_ = timestamp;
    return output;
}
