#pragma once

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "account.h"
#include "decision_batch.h"
#include "exchange.h"
#include "order_manager.h"
#include "order_planning.h"
#include "order_planner_engine.h"
#include "strategy_instance.h"
#include "strategy_position_snapshot.h"
#include "target_portfolio.h"
#include "trade_recorder.h"


struct ExecutionStrategyDescriptor {
    StrategyID strategy_id = 0;
    std::string name;
    VirtualPositionState initial_positions;
};


/**************************************************************************************
 * Type    : ExecutionEngine
 * Purpose : Own executable account/order state and route exchange lifecycle events
 *
 * RebalancePlan remains decision intent. Quantity is still resolved only when executable
 * reference prices are supplied, preserving the validated close-T -> open-T+1 semantics.
 *
 * StrategyPositionSnapshot is authoritative here. StrategyInstance may retain a temporary
 * compatibility mirror, but Decision only receives a read-only execution snapshot.
 **************************************************************************************/
class ExecutionEngine {
public:
    using PersistCallback = std::function<void(const std::optional<Fill>&)>;

private:
    Account& account_;
    TradeRecorder& trade_recorder_;
    Exchange& exchange_;

    std::vector<StrategyID> strategy_ids_;
    std::unordered_map<StrategyID, std::string> strategy_names_;
    StrategyPositionSnapshot strategy_positions_;

    OrderManager order_manager_;
    OrderPlannerEngine order_planner_;

    TargetPortfolio last_global_target_;
    OrderID next_order_id_ = 1;
    Timestamp last_execution_timestamp_ = 0;

    VirtualPositionState& strategyPositionById(StrategyID strategyId);
    const std::string& strategyNameById(StrategyID strategyId) const;
    void applyFill(const Fill& fill, const PersistCallback& persist);

public:
    ExecutionEngine(
        const StrategyPortfolio& strategies,
        Account& account,
        TradeRecorder& tradeRecorder,
        Exchange& exchange
    );

    ExecutionEngine(
        const std::vector<ExecutionStrategyDescriptor>& strategies,
        Account& account,
        TradeRecorder& tradeRecorder,
        Exchange& exchange
    );

    double accountEquity(const PriceSnapshot& marks) const
    {
        return account_.equity(marks);
    }

    void applyOrderPlan(
        const OrderPlanBatch& plan,
        const PersistCallback& persist
    );

    void executeDecisionBatch(
        Timestamp ts,
        const ExecutionReferencePrices& prices,
        const DecisionBatch& decisions,
        const PersistCallback& persist
    );

    // Process exactly one externally-delivered exchange event. Distributed runtimes use
    // this method so a durable transport ACK can happen only after persistence succeeds.
    void processExchangeEvent(
        const ExchangeEvent& event,
        const PersistCallback& persist
    );

    void processExchangeEvents(const PersistCallback& persist);

    Account& account() { return account_; }
    const Account& account() const { return account_; }

    OrderManager& orderManager() { return order_manager_; }
    const OrderManager& orderManager() const { return order_manager_; }

    const StrategyPositionSnapshot& strategyPositions() const
    {
        return strategy_positions_;
    }

    const VirtualPositionState& strategyPosition(StrategyID strategyId) const;

    const TargetPortfolio& lastGlobalTarget() const { return last_global_target_; }
    OrderID nextOrderId() const { return next_order_id_; }
    Timestamp lastExecutionTimestamp() const { return last_execution_timestamp_; }

    void restoreState(
        double accountCash,
        const std::unordered_map<Coin, double>& accountPositions,
        const StrategyPositionSnapshot& strategyPositions,
        const std::vector<TrackedOrder>& orders,
        const std::vector<FillID>& processedFillIds,
        OrderID nextOrderId,
        Timestamp lastExecutionTimestamp
    );
};
