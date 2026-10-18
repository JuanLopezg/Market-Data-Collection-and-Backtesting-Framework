// Standby plan: the orders the deployed chain would send at open(T+1), computed locally.
//
// standby/standby.py writes a canonical market SQLite (the market-data service's schema,
// built from Binance daily klines) and a manifest, then runs this program. It runs the
// deployed profile split as the chain runs it, with the same lib components as the
// canonical replay (ReplayRuntime):
//   1. strategy: StrategySignalEngine over each settle day and T, with the strategy
//      service's window (top N plus active signals, warmup days). Signals do not depend
//      on the account, so replaying them from a cold start settles the signal state;
//   2. portfolio risk at close(T): PortfolioRiskEngine with the real account (cash on the
//      spot convention of execution-state, venue collateral, positions in source units)
//      and the risk service's window (signalled and held symbols, lookback + buffer days);
//   3. planning: NotionalOrderPlanner priced at the exact close(T), as execution-state
//      asks the planner. Quantities are resolved at the next open by the venue path; the
//      plan shows them at close(T) only as an estimate.
// Nothing is filled or sent.
//
// Usage: algotrading_standby_plan <manifest.json>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "account_snapshot.h"
#include "canonical_market_data_reader.h"
#include "chain_profile.h"
#include "logger.h"
#include "planning/notional_order_planner.h"
#include "order_manager.h"
#include "portfolio_risk_engine.h"
#include "strategy_signal_engine.h"
#include "time_utils.h"

namespace {

using json = nlohmann::json;

std::set<Coin> activeSignals(const StrategyIntentBatch& batch)
{
    std::set<Coin> active;
    for (const StrategySignalIntent& strategy : batch.strategies)
        for (const auto& [coin, signal] : strategy.signals)
            if (signal != 0.0) active.insert(coin);
    return active;
}

// portfolio_risk_service requiredSymbols: every signalled or held symbol.
std::set<Coin> riskSymbols(const StrategyIntentBatch& batch, const AccountSnapshot& account)
{
    std::set<Coin> symbols;
    for (const StrategySignalIntent& strategy : batch.strategies)
        for (const auto& [coin, signal] : strategy.signals) symbols.insert(coin);
    for (const auto& [coin, quantity] : account.positions) symbols.insert(coin);
    for (const auto& [strategyId, positions] : account.strategy_positions)
        for (const auto& [coin, quantity] : positions) symbols.insert(coin);
    return symbols;
}

const char* actionName(RebalanceAction action)
{
    switch (action) {
    case RebalanceAction::Hold: return "HOLD";
    case RebalanceAction::Flat: return "EXIT";
    case RebalanceAction::TargetWeight: return "TARGET";
    }
    return "UNKNOWN";
}

} // namespace

int main(int argc, char** argv)
try {
    if (argc != 2) {
        std::cerr << "Usage: algotrading_standby_plan <manifest.json>\n";
        return 2;
    }
    Logger::Instance().Setup(false, false); // INFO, stdout only
    const json input = chain_profile::readJson(argv[1]);
    const json strategyFile = chain_profile::readJson(input.at("strategy_config"));
    const json portfolioFile = chain_profile::readJson(input.at("portfolio_config"));
    const Timestamp decisionDate = input.at("decision_date");
    const unsigned warmupDays = input.at("warmup_days");
    const unsigned topN = input.at("top_n");
    const CanonicalMarketDataReader reader(input.at("database").get<std::string>());

    // 1. Strategy signals: each settle day, then T.
    StrategySignalEngine signals(chain_profile::makeSignalPortfolio(strategyFile));
    std::vector<Timestamp> days = input.at("settle_days").get<std::vector<Timestamp>>();
    days.push_back(decisionDate);
    StrategyIntentBatch batch;
    std::size_t rankedSymbols = 0;
    for (std::size_t index = 0; index < days.size(); ++index) {
        if (index > 0 && days[index] <= days[index - 1])
            throw std::runtime_error("Settle days must be ordered and before the decision date");
        const CanonicalMarketDataWindow window = reader.loadWindow(days[index], warmupDays, topN, activeSignals(batch));
        batch = signals.onBarClose(window.raw_data, window.market_data, days[index]);
        rankedSymbols = window.ranked_symbols;
    }

    // 2. Portfolio risk at close(T) with the real account. One strategy owns the whole
    //    account, so its strategy positions are the account positions.
    const json& real = input.at("account");
    AccountSnapshot account;
    account.timestamp = decisionDate;
    account.cash = real.at("cash").get<double>();
    account.collateral_usd = real.value("collateral_usd", 0.0);
    account.positions = real.at("positions").get<std::unordered_map<Coin, double>>();
    std::vector<StrategyID> strategyIds;
    for (const StrategySignalIntent& strategy : batch.strategies) strategyIds.push_back(strategy.strategy_id);
    if (strategyIds.size() != 1)
        throw std::runtime_error("The standby plan supports one strategy owning the whole account");
    account.strategy_positions[strategyIds.front()] = account.positions;

    PortfolioRiskEngine risk(chain_profile::makeRiskConfigs(portfolioFile));
    const std::set<Coin> symbols = riskSymbols(batch, account);
    MarketData riskData;
    if (symbols.empty())
        riskData.emplace(decisionDate, CoinBarMap{});
    else
        riskData = reader.loadMarketDataWindow(decisionDate, chain_profile::riskHistoryDays(portfolioFile, 5), symbols);
    const DecisionBatch decisions = risk.onSignals(batch, riskData, account);

    // 3. Notional plan priced at the exact close(T) of the held and decided symbols.
    std::set<Coin> planningSymbols;
    for (const auto& [coin, quantity] : account.positions) planningSymbols.insert(coin);
    for (const StrategyDecisionIntent& strategy : decisions.strategies)
        for (const auto& [coin, decision] : strategy.decisions) planningSymbols.insert(coin);
    ExecutionReferencePrices closes;
    for (const auto& [coin, price] : reader.loadExactClosingPrices(decisionDate, planningSymbols))
        closes.set(coin, price);
    StrategyPositionSnapshot strategyPositions;
    VirtualPositionState positions;
    for (const auto& [coin, quantity] : account.positions) positions.set(coin, quantity);
    strategyPositions.emplace(strategyIds.front(), positions);
    const OrderManager noOpenOrders;
    const NotionalOrderPlannerResult plan = NotionalOrderPlanner().createPlan(
        strategyIds, strategyPositions, noOpenOrders, decisionDate, 1, closes, decisions, 1);

    json orders = json::array();
    for (const PlannedNotionalOrder& order : plan.submit_orders)
        orders.push_back({{"coin", order.coin},
                          {"side", order.side == OrderSide::Buy ? "BUY" : "SELL"},
                          {"notional_usd", order.notional_usd},
                          {"reference_close", order.reference_close},
                          {"quantity_at_close", order.reference_close > 0.0 ? order.notional_usd / order.reference_close : 0.0},
                          {"current_notional_usd", order.current_notional_usd},
                          {"target_notional_usd", order.target_notional_usd}});
    json decided = json::array();
    double referenceCapital = 0.0;
    for (const StrategyDecisionIntent& strategy : decisions.strategies) {
        referenceCapital += strategy.reference_capital;
        for (const auto& [coin, decision] : strategy.decisions) {
            const auto target = strategy.target_notional_usd.find(coin);
            decided.push_back({{"coin", coin}, {"action", actionName(decision.action)},
                               {"target_weight", decision.target_weight},
                               {"target_notional_usd", target == strategy.target_notional_usd.end() ? 0.0 : target->second}});
        }
    }

    const json output{{"contractVersion", "standby-plan-v2"},
                      {"decisionDate", decisionDate},
                      {"executionDate", nextDay(decisionDate)},
                      {"rankedSymbols", rankedSymbols},
                      {"signals", batch.strategies.front().signals},
                      {"referenceCapital", referenceCapital},
                      {"decisions", decided},
                      {"orders", orders}};
    std::ofstream stream(input.at("output").get<std::string>());
    if (!stream) throw std::runtime_error("Cannot write the standby plan");
    stream << output.dump(2) << '\n';
    LG_INFO("Standby plan for close {}: {} decisions, {} orders for the open of {}",
            decisionDate, decided.size(), orders.size(), nextDay(decisionDate));
    return 0;
}
catch (const std::exception& error) {
    std::cerr << "Standby plan failed: " << error.what() << '\n';
    return 1;
}
