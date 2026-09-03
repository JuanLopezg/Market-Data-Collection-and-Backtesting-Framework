#pragma once

#include <optional>
#include <unordered_map>
#include <vector>

#include "decision_engine.h"
#include "execution_engine.h"
#include "state_store.h"
#include "trading_state_snapshot.h"


/**************************************************************************************
 * Type    : TradingEngine
 * Purpose : Backward-compatible in-process facade over DecisionEngine + ExecutionEngine
 *
 * Runtimes keep the validated API while strategy decision work and executable order/fill
 * work are now delegated to explicit engines. This is the first structural split before
 * transport DTOs and distributed service boundaries are introduced.
 **************************************************************************************/
class TradingEngine {
private:
    StrategyPortfolio& strategies_;
    TradeRecorder& trade_recorder_;

    DecisionEngine decision_engine_;
    ExecutionEngine execution_engine_;

    StateStore* state_store_ = nullptr; // Non-owning; live runtime controls store lifetime.
    bool trading_enabled_ = true;

    StrategyInstance& strategyById(StrategyID strategyId);
    void syncStrategyPositionMirrors();
    void requireTradingEnabled() const;
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

    bool hasPendingPlans() const { return decision_engine_.hasPendingDecisions(); }

    OrderManager& orderManager() { return execution_engine_.orderManager(); }
    const OrderManager& orderManager() const { return execution_engine_.orderManager(); }

    const TargetPortfolio& lastGlobalTarget() const
    {
        return execution_engine_.lastGlobalTarget();
    }
};
