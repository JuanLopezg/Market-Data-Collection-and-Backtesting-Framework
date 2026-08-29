#pragma once

#include <optional>
#include <unordered_map>
#include <vector>

#include "account.h"
#include "account_target_resolver.h"
#include "exchange.h"
#include "execution_coordinator.h"
#include "indicator_engine.h"
#include "order_manager.h"
#include "portfolio_aggregator.h"
#include "rebalance_plan.h"
#include "state_store.h"
#include "strategy_instance.h"
#include "strategy_target_resolver.h"
#include "target_portfolio.h"
#include "trade_recorder.h"
#include "trading_state_snapshot.h"


/**************************************************************************************
 * Type    : TradingEngine
 * Purpose : Shared strategy/execution orchestrator used by backtest and future live runtime
 *
 * TradingEngine deliberately knows nothing about historical bars, next-open semantics,
 * wall-clock scheduling or exchange APIs. A runtime supplies market events/reference
 * prices; the engine owns pending plans, order lifecycle and fill application.
 **************************************************************************************/
class TradingEngine {
private:
    StrategyPortfolio& strategies_;
    Account& account_;
    TradeRecorder& trade_recorder_;
    const IndicatorEngine& indicators_;
    Exchange& exchange_;

    ExecutionCoordinator execution_coordinator_;
    OrderManager order_manager_;
    PortfolioAggregator portfolio_aggregator_;
    StrategyTargetResolver strategy_target_resolver_;
    AccountTargetResolver account_target_resolver_;

    std::unordered_map<StrategyID, RebalancePlan> pending_plans_;
    TargetPortfolio last_global_target_;
    OrderID next_order_id_ = 1;

    Timestamp last_bar_close_timestamp_ = 0;
    Timestamp last_execution_timestamp_ = 0;
    StateStore* state_store_ = nullptr; // Non-owning; live runtime controls store lifetime.
    bool trading_enabled_ = true;

    StrategyInstance& strategyById(StrategyID strategyId);
    void requireTradingEnabled() const;
    void applyFill(const Fill& fill);
    void persist(const std::optional<Fill>& newFill = std::nullopt) const;

public:
    TradingEngine(
        StrategyPortfolio& strategies,
        Account& account,
        TradeRecorder& tradeRecorder,
        const IndicatorEngine& indicators,
        Exchange& exchange
    );

    // Persistence is optional. Backtests can remain completely in-memory.
    void attachStateStore(StateStore& stateStore);
    void checkpoint() const;
    TradingStateSnapshot stateSnapshot() const;

    // Startup recovery restores operational state without replaying external side effects.
    void restoreState(const TradingStateSnapshot& snapshot);
    void rebuildTradeRecorder(const std::vector<Fill>& fills);

    // Recovery/reconciliation may pause new strategy/execution work while still allowing
    // queued exchange events to be processed. Backtests start enabled by default.
    void pauseTrading() { trading_enabled_ = false; }
    void resumeTrading() { trading_enabled_ = true; }
    bool tradingEnabled() const { return trading_enabled_; }

    // Completed market-data event: update signals and create plans for a later execution event.
    void onBarClose(
        const MarketData& marketData,
        Timestamp ts,
        const PriceSnapshot& marks
    );

    // Resolve pending strategy plans at the supplied executable/reference prices.
    void executePendingPlans(
        Timestamp ts,
        const ExecutionReferencePrices& prices
    );

    // Apply queued asynchronous exchange events in their original order.
    void processExchangeEvents();

    bool hasPendingPlans() const { return !pending_plans_.empty(); }

    OrderManager& orderManager() { return order_manager_; }
    const OrderManager& orderManager() const { return order_manager_; }

    const TargetPortfolio& lastGlobalTarget() const { return last_global_target_; }
};
