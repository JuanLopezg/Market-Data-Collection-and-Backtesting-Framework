#pragma once

#include <cassert>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "data_types.h"
#include "strategy_instance.h"
#include "indicator_engine.h"


/**************************************************************************************
 * Type    : BacktestContext
 * Purpose : Holds global backtest state and shared execution context
 **************************************************************************************/
class BacktestContext {
private:
    MarketData market_data_;

    IndicatorEngine indicator_engine_;

    StrategyPortfolio strategy_portfolio_;

    double current_balance_;
    double current_equity_;

    unsigned int last_trade_id_;

    double commission_limit_factor_;
    double commission_market_factor_;

    std::vector<std::pair<Balance, Equity>> balance_equity_historic_;

    std::map<TradeID, Trade> trades_history_;

    bool live_trading_ = false;

public:
    explicit BacktestContext(
        const OHLCVData& rawData,
        std::vector<std::unique_ptr<Strategy>>& strategies,
        double initialBalance,
        double initialEquity,
        unsigned int lastTradeId = 0,
        double commissionLimitFactor = 0.0,
        double commissionMarketFactor = 0.0,
        bool liveTrading = false
    )
        : market_data_(buildMarketData(rawData)),
          indicator_engine_(),
          current_balance_(initialBalance),
          current_equity_(initialEquity),
          last_trade_id_(lastTradeId),
          commission_limit_factor_(commissionLimitFactor),
          commission_market_factor_(commissionMarketFactor),
          live_trading_(liveTrading)
    {
        assert(!rawData.data.empty());
        assert(!market_data_.empty());
        assert(!strategies.empty());
        assert(initialBalance > 0.0);
        assert(initialEquity > 0.0);
        assert(commissionLimitFactor  >= 0.0 && commissionLimitFactor  <= 100.0);
        assert(commissionMarketFactor >= 0.0 && commissionMarketFactor <= 100.0);

        const double weight = 1.0 / static_cast<double>(strategies.size());

        for (auto& strategy : strategies) {
            strategy_portfolio_.emplace_back(
                current_balance_,
                current_equity_,
                std::move(strategy),
                weight
            );
        }

        std::vector<IndicatorSpec> requiredSpecs;

        for (const auto& strategyInstance : strategy_portfolio_) {
            const auto specs =
                strategyInstance.GetStrategy().requiredIndicators();

            requiredSpecs.insert(
                requiredSpecs.end(),
                specs.begin(),
                specs.end()
            );
        }

        indicator_engine_.precompute(rawData, requiredSpecs);
    }

    void updateConext();

    void updateContext()
    {
        updateConext();
    }

    const MarketData& GetMarketData() const
    {
        return market_data_;
    }

    StrategyPortfolio& GetStrategyPortfolio()
    {
        return strategy_portfolio_;
    }

    const StrategyPortfolio& GetStrategyPortfolio() const
    {
        return strategy_portfolio_;
    }

    double& GetCurrentBalance()
    {
        return current_balance_;
    }

    const double& GetCurrentBalance() const
    {
        return current_balance_;
    }

    double& GetCurrentEquity()
    {
        return current_equity_;
    }

    const double& GetCurrentEquity() const
    {
        return current_equity_;
    }

    unsigned int& GetLastTradeId()
    {
        return last_trade_id_;
    }

    const unsigned int& GetLastTradeId() const
    {
        return last_trade_id_;
    }

    std::vector<std::pair<Balance, Equity>>& GetBalanceEquityHistoric()
    {
        return balance_equity_historic_;
    }

    const std::vector<std::pair<Balance, Equity>>& GetBalanceEquityHistoric() const
    {
        return balance_equity_historic_;
    }

    std::map<TradeID, Trade>& GetTradesHistory()
    {
        return trades_history_;
    }

    const std::map<TradeID, Trade>& GetTradesHistory() const
    {
        return trades_history_;
    }

    double& GetCommissionLimitFactor()
    {
        return commission_limit_factor_;
    }

    const double& GetCommissionLimitFactor() const
    {
        return commission_limit_factor_;
    }

    double& GetCommissionMarketFactor()
    {
        return commission_market_factor_;
    }

    const double& GetCommissionMarketFactor() const
    {
        return commission_market_factor_;
    }

    bool IsLiveTrading() const
    {
        return live_trading_;
    }

    bool& GetLiveTradingFlag()
    {
        return live_trading_;
    }

    const IndicatorEngine& GetIndicatorEngine() const
    {
        return indicator_engine_;
    }

    IndicatorEngine& GetIndicatorEngine()
    {
        return indicator_engine_;
    }
};