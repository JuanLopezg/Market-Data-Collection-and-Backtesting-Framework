#pragma once

#include <cassert>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include "account.h"
#include "data_types.h"
#include "indicator_engine.h"
#include "price_snapshot.h"
#include "strategy_instance.h"
#include "target_portfolio.h"
#include "trade_recorder.h"
#include "volatility_diagnostic.h"


struct AccountSnapshot {
    Timestamp timestamp = 0;
    double cash = 0.0;
    Balance balance = 0.0;
    Equity equity = 0.0;
};


/**************************************************************************************
 * Type    : BacktestContext
 * Purpose : Shared historical data, configured strategies and actual simulated account
 *
 * BacktestContext no longer derives account state from Trade objects. Account changes
 * from Fill objects; TradeRecord is analytics reconstructed separately from fills.
 **************************************************************************************/
class BacktestContext {
private:
    MarketData market_data_;
    IndicatorEngine indicator_engine_;
    StrategyPortfolio strategy_portfolio_;

    Account account_;
    double initial_capital_ = 0.0;
    double market_commission_rate_ = 0.0;

    std::vector<AccountSnapshot> account_history_;
    std::vector<std::pair<Balance, Equity>> balance_equity_historic_;
    TradeRecorder trade_recorder_;
    std::vector<VolatilityDiagnosticSnapshot> volatility_diagnostics_;
    mutable std::map<TradeID, Trade> legacy_trade_cache_;

    TargetPortfolio last_global_target_;
    PriceSnapshot last_marks_;
    Timestamp last_timestamp_ = 0;

public:
    BacktestContext(
        const OHLCVData& rawData,
        StrategyPortfolio strategies,
        double initialCash,
        double marketCommissionRate = 0.0
    )
        : market_data_(buildMarketData(rawData)),
          strategy_portfolio_(std::move(strategies)),
          account_(initialCash),
          initial_capital_(initialCash),
          market_commission_rate_(marketCommissionRate)
    {
        assert(!rawData.data.empty());
        assert(!market_data_.empty());

        if (strategy_portfolio_.empty())
            throw std::invalid_argument("Backtest requires at least one strategy instance");
        if (!std::isfinite(market_commission_rate_) || market_commission_rate_ < 0.0)
            throw std::invalid_argument("Market commission rate must be finite and non-negative");

        std::set<StrategyID> ids;
        double allocationSum = 0.0;
        std::vector<IndicatorSpec> requiredSpecs;

        for (const auto& strategyInstance : strategy_portfolio_) {
            if (!ids.insert(strategyInstance.id()).second)
                throw std::invalid_argument("Strategy ids must be unique");

            allocationSum += strategyInstance.allocationWeight();

            const auto specs = strategyInstance.strategy().requiredIndicators();
            requiredSpecs.insert(requiredSpecs.end(), specs.begin(), specs.end());
        }

        if (!std::isfinite(allocationSum) || allocationSum > 1.0 + 1e-12)
            throw std::invalid_argument("Strategy allocation weights cannot exceed 100% in total");

        indicator_engine_.precompute(rawData, requiredSpecs);
    }

    StrategyInstance& strategyById(StrategyID strategyId)
    {
        for (auto& strategy : strategy_portfolio_) {
            if (strategy.id() == strategyId)
                return strategy;
        }

        throw std::out_of_range("Strategy id not found");
    }

    const MarketData& GetMarketData() const { return market_data_; }
    StrategyPortfolio& GetStrategyPortfolio() { return strategy_portfolio_; }
    const StrategyPortfolio& GetStrategyPortfolio() const { return strategy_portfolio_; }

    Account& GetAccount() { return account_; }
    const Account& GetAccount() const { return account_; }

    const IndicatorEngine& GetIndicatorEngine() const { return indicator_engine_; }
    IndicatorEngine& GetIndicatorEngine() { return indicator_engine_; }

    TradeRecorder& GetTradeRecorder() { return trade_recorder_; }
    const TradeRecorder& GetTradeRecorder() const { return trade_recorder_; }

    std::vector<AccountSnapshot>& GetAccountHistory() { return account_history_; }
    const std::vector<AccountSnapshot>& GetAccountHistory() const { return account_history_; }

    void recordVolatilityDiagnostic(
        Timestamp ts,
        StrategyID strategyId,
        const std::string& strategyName,
        const PortfolioSizingDiagnostics& sizingDiagnostics
    )
    {
        volatility_diagnostics_.push_back({
            ts,
            strategyId,
            strategyName,
            sizingDiagnostics
        });
    }

    const std::vector<VolatilityDiagnosticSnapshot>& GetVolatilityDiagnostics() const
    {
        return volatility_diagnostics_;
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
        return const_cast<std::map<TradeID, Trade>&>(
            static_cast<const BacktestContext&>(*this).GetTradesHistory()
        );
    }

    const std::map<TradeID, Trade>& GetTradesHistory() const
    {
        legacy_trade_cache_.clear();

        if (last_marks_.size() == 0)
            return legacy_trade_cache_;

        const auto records = trade_recorder_.allTrades(last_marks_, last_timestamp_);
        for (const TradeRecord& record : records) {
            Trade trade;
            trade.trade_id_ = record.trade_id;
            trade.start_ = record.start;
            trade.end_ = record.end;
            trade.commission_ = record.commission;
            trade.coin_ = record.coin;
            trade.direction_ = record.direction;
            trade.current_price_ = record.exit_price;
            trade.entry_ = record.entry_price;
            trade.exit_ = record.exit_price;
            trade.size_ = record.peak_quantity;
            trade.pnl_ = record.pnl;
            trade.isSimulated_ = false;
            trade.exited_ = record.exited;
            trade.strategy_name_ = record.strategy_name;
            legacy_trade_cache_[trade.trade_id_] = std::move(trade);
        }

        return legacy_trade_cache_;
    }

    double GetCommissionMarketRate() const { return market_commission_rate_; }

    void setLastGlobalTarget(TargetPortfolio target)
    {
        last_global_target_ = std::move(target);
    }

    const TargetPortfolio& GetLastGlobalTarget() const
    {
        return last_global_target_;
    }

    void recordAccountSnapshot(Timestamp ts, const PriceSnapshot& marks)
    {
        last_timestamp_ = ts;
        last_marks_ = marks;

        const double equity = account_.equity(marks);
        const double balance = initial_capital_ + trade_recorder_.cumulativeClosedPnl();

        account_history_.push_back({
            ts,
            account_.cash(),
            balance,
            equity
        });

        // Backtest chart semantics:
        //   Balance = initial capital + PnL from fully closed trades only.
        //   Equity  = marked account value, including open/unrealized PnL.
        balance_equity_historic_.emplace_back(balance, equity);
    }

    const PriceSnapshot& GetLastMarks() const { return last_marks_; }
    Timestamp GetLastTimestamp() const { return last_timestamp_; }

    double GetCurrentBalance() const
    {
        return initial_capital_ + trade_recorder_.cumulativeClosedPnl();
    }

    double GetCurrentCash() const
    {
        return account_.cash();
    }

    double GetCurrentEquity() const
    {
        if (last_marks_.size() == 0)
            return account_.cash();

        return account_.equity(last_marks_);
    }
};
