#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "data_types.h"
#include "portfolio_risk_engine.h"
#include "strategy_instance.h"
#include "strategy_signal_instance.h"

// The deployed chain's trading profile, built in process from the same JSON files the
// services read (config/strategies/<STRATEGY_FILE>, config/portfolio/<PORTFOLIO_FILE>).
// Sizer and rebalance construction match live_trading/portfolio_risk_service
// (makeSizer/makeRebalancePolicy). Used by the PAPER/LIVE fast baseline of comparison.py
// and by the standby plan (standby/), so both run exactly the deployed profile.
namespace chain_profile {

nlohmann::json readJson(const std::string& path);

// One PureRSI strategy over the QuoteVolume liquidity universe (the only deployed
// profile); strategy and portfolio are the first entry of each file's "strategies".
StrategyPortfolio makeStrategyPortfolio(const nlohmann::json& strategy, const nlohmann::json& portfolio);

// The service split of the same profile, as the chain runs it: strategy signals
// (strategy_service loadStrategies) and portfolio risk (portfolio_risk_service
// loadPortfolioConfig). Both take the whole file ({"strategies": [...]}).
StrategySignalPortfolio makeSignalPortfolio(const nlohmann::json& strategyFile);
std::vector<PortfolioRiskStrategyConfig> makeRiskConfigs(const nlohmann::json& portfolioFile);

// Days of market history portfolio risk loads (requiredMarketHistoryDays): the largest
// covariance lookback + 1 + bufferDays (5 in the chain), or 1 with equal weight only.
unsigned int riskHistoryDays(const nlohmann::json& portfolioFile, unsigned int bufferDays);

// Days of close history the sizer needs, counted like the risk service
// (requiredMarketHistoryDays): lookback returns + 1 + 5 buffer days; 0 for equal weight.
std::size_t sizerHistoryDays(const nlohmann::json& sizer);

// A canonical window's MarketData holds only the decision day. The volatility sizer needs
// the preceding closes too, as the risk service reads them: add the earlier days from the
// window's raw history. The decision day itself is left exactly as loaded.
void addSizerHistory(MarketData& marketData, const OHLCVData& raw, Timestamp closeDate, std::size_t historyDays);

} // namespace chain_profile
