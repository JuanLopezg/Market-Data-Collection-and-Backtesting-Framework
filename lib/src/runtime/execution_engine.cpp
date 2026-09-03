#include "execution_engine.h"

#include <algorithm>
#include <unordered_set>
#include <stdexcept>
#include <utility>


namespace {

std::vector<ExecutionStrategyDescriptor> executionDescriptors(
    const StrategyPortfolio& strategies
)
{
    std::vector<ExecutionStrategyDescriptor> result;
    result.reserve(strategies.size());

    for (const auto& strategy : strategies) {
        result.push_back({
            strategy.id(),
            strategy.name(),
            strategy.virtualPositions()
        });
    }

    return result;
}

} // namespace


ExecutionEngine::ExecutionEngine(
    const StrategyPortfolio& strategies,
    Account& account,
    TradeRecorder& tradeRecorder,
    Exchange& exchange
)
    : ExecutionEngine(
        executionDescriptors(strategies),
        account,
        tradeRecorder,
        exchange
    )
{}


ExecutionEngine::ExecutionEngine(
    const std::vector<ExecutionStrategyDescriptor>& strategies,
    Account& account,
    TradeRecorder& tradeRecorder,
    Exchange& exchange
)
    : account_(account),
      trade_recorder_(tradeRecorder),
      exchange_(exchange)
{
    if (strategies.empty())
        throw std::invalid_argument("ExecutionEngine requires at least one strategy descriptor");

    strategy_ids_.reserve(strategies.size());

    for (const ExecutionStrategyDescriptor& strategy : strategies) {
        if (strategy.name.empty())
            throw std::invalid_argument("Execution strategy name cannot be empty");
        if (!strategy_names_.emplace(strategy.strategy_id, strategy.name).second)
            throw std::invalid_argument("Duplicate strategy id in ExecutionEngine");

        strategy_ids_.push_back(strategy.strategy_id);
        strategy_positions_.emplace(strategy.strategy_id, strategy.initial_positions);
    }
}


VirtualPositionState& ExecutionEngine::strategyPositionById(StrategyID strategyId)
{
    const auto it = strategy_positions_.find(strategyId);
    if (it == strategy_positions_.end())
        throw std::out_of_range("Strategy id not found in execution position state");

    return it->second;
}


const VirtualPositionState& ExecutionEngine::strategyPosition(StrategyID strategyId) const
{
    const auto it = strategy_positions_.find(strategyId);
    if (it == strategy_positions_.end())
        throw std::out_of_range("Strategy id not found in execution position state");

    return it->second;
}


const std::string& ExecutionEngine::strategyNameById(StrategyID strategyId) const
{
    const auto it = strategy_names_.find(strategyId);
    if (it == strategy_names_.end())
        throw std::out_of_range("Strategy id not found");

    return it->second;
}


/**************************************************************************************
 * Purpose : Apply one actual fill to lifecycle/account/strategy/analytics state
 **************************************************************************************/
void ExecutionEngine::applyFill(const Fill& fill, const PersistCallback& persist)
{
    // Validate/idempotently record the fill before mutating account state.
    if (!order_manager_.onFill(fill))
        return; // A replayed FillID must not mutate account/strategy/analytics twice.

    account_.applyFill(fill);
    strategyPositionById(fill.strategy_id).add(fill.coin, fill.signedQuantity());
    trade_recorder_.onFill(fill, strategyNameById(fill.strategy_id));

    // Fill audit row + resulting operational snapshot commit atomically.
    persist(fill);
}


/**************************************************************************************
 * Purpose : Process one exchange lifecycle event and persist before returning
 **************************************************************************************/
void ExecutionEngine::processExchangeEvent(
    const ExchangeEvent& event,
    const PersistCallback& persist
)
{
    if (const auto* update = std::get_if<OrderUpdate>(&event)) {
        order_manager_.onOrderUpdate(*update);
        persist(std::nullopt);
        return;
    }

    if (const auto* fill = std::get_if<Fill>(&event))
        applyFill(*fill, persist);
}


/**************************************************************************************
 * Purpose : Route queued local exchange events through the same single-event path
 **************************************************************************************/
void ExecutionEngine::processExchangeEvents(const PersistCallback& persist)
{
    const std::vector<ExchangeEvent> events = exchange_.drainEvents();

    for (const ExchangeEvent& event : events)
        processExchangeEvent(event, persist);
}


/**************************************************************************************
 * Purpose : Accept a previously-computed order plan and own all resulting side effects
 *
 * All local cancel/submit intents are persisted together before the first outbound
 * exchange command. This makes a crash in the middle of command publication recoverable:
 * Created orders and cancel_requested flags remain visible to reconciliation/recovery.
 **************************************************************************************/
void ExecutionEngine::applyOrderPlan(
    const OrderPlanBatch& plan,
    const PersistCallback& persist
)
{
    if (plan.execution_timestamp == 0)
        throw std::invalid_argument("Order plan execution timestamp must be non-zero");
    if (plan.next_order_id == 0 || plan.next_order_id < next_order_id_)
        throw std::invalid_argument("Order plan next order id regresses execution state");

    std::unordered_set<OrderID> cancelIds;
    for (const OrderID orderId : plan.cancel_order_ids) {
        if (!cancelIds.insert(orderId).second)
            throw std::invalid_argument("Order plan contains duplicate cancel order id");

        const TrackedOrder* tracked = order_manager_.find(orderId);
        if (!tracked || !tracked->isOpen() || tracked->cancel_requested)
            throw std::invalid_argument("Order plan contains invalid cancel intent");
    }

    std::unordered_set<OrderID> submitIds;
    for (const ExecutionOrder& order : plan.submit_orders) {
        if (!submitIds.insert(order.order_id).second)
            throw std::invalid_argument("Order plan contains duplicate submit order id");
        if (order.order_id < next_order_id_ || order.order_id >= plan.next_order_id)
            throw std::invalid_argument("Order plan submit id is outside allocated order-id range");
        if (order_manager_.find(order.order_id))
            throw std::invalid_argument("Order plan submit id is already tracked");
        if (strategy_names_.find(order.strategy_id) == strategy_names_.end())
            throw std::invalid_argument("Order plan contains unknown strategy id");
    }

    last_execution_timestamp_ = plan.execution_timestamp;
    next_order_id_ = plan.next_order_id;
    last_global_target_.clear();
    for (const auto& [coin, exposure] : plan.global_target_exposure)
        last_global_target_.set(coin, exposure);

    // Stage every local intent first, then commit one complete accepted-plan snapshot.
    for (const OrderID orderId : plan.cancel_order_ids)
        order_manager_.markCancelRequested(orderId, plan.execution_timestamp);
    for (const ExecutionOrder& order : plan.submit_orders)
        order_manager_.track(order);
    persist(std::nullopt);

    // Only after the full local plan is durable may commands leave this process.
    for (const OrderID orderId : plan.cancel_order_ids)
        exchange_.cancelOrder(orderId);

    for (const ExecutionOrder& order : plan.submit_orders) {
        exchange_.submitOrder(order);
        order_manager_.markSubmitted(order.order_id, plan.execution_timestamp);
        persist(std::nullopt);
    }
}


/**************************************************************************************
 * Purpose : Resolve decision intent and apply the resulting order plan in-process
 **************************************************************************************/
void ExecutionEngine::executeDecisionBatch(
    Timestamp ts,
    const ExecutionReferencePrices& prices,
    const DecisionBatch& decisions,
    const PersistCallback& persist
)
{
    OrderPlanBatch plan;
    plan.decision_timestamp = decisions.decision_timestamp;
    plan.execution_timestamp = ts;
    plan.next_order_id = next_order_id_;

    if (!decisions.strategies.empty()) {
        const OrderPlannerResult planning = order_planner_.createPlan(
            strategy_ids_,
            strategy_positions_,
            order_manager_,
            ts,
            prices,
            decisions,
            next_order_id_
        );

        plan.next_order_id = planning.next_order_id;
        plan.cancel_order_ids = planning.execution_plan.order_ids_to_cancel;
        plan.submit_orders = planning.execution_plan.orders_to_submit;
        plan.global_target_exposure = planning.global_target.values();
    }

    applyOrderPlan(plan, persist);
}

void ExecutionEngine::restoreState(
    double accountCash,
    const std::unordered_map<Coin, double>& accountPositions,
    const StrategyPositionSnapshot& strategyPositions,
    const std::vector<TrackedOrder>& orders,
    const std::vector<FillID>& processedFillIds,
    OrderID nextOrderId,
    Timestamp lastExecutionTimestamp
)
{
    if (nextOrderId == 0)
        throw std::invalid_argument("Restored next order id must be non-zero");

    OrderID maximumOrderId = 0;
    for (const TrackedOrder& order : orders)
        maximumOrderId = std::max(maximumOrderId, order.request.order_id);

    if (nextOrderId <= maximumOrderId)
        throw std::invalid_argument("Restored next order id does not follow tracked orders");

    StrategyPositionSnapshot restoredPositions;
    for (const StrategyID strategyId : strategy_ids_) {
        const auto it = strategyPositions.find(strategyId);
        if (it == strategyPositions.end())
            restoredPositions.emplace(strategyId, VirtualPositionState{});
        else
            restoredPositions.emplace(strategyId, it->second);
    }

    for (const auto& [strategyId, positions] : strategyPositions) {
        (void)positions;
        if (strategy_names_.find(strategyId) == strategy_names_.end())
            throw std::invalid_argument("Restored execution positions contain unknown strategy id");
    }

    account_.restoreState(accountCash, accountPositions);
    strategy_positions_ = std::move(restoredPositions);
    order_manager_.restore(orders, processedFillIds);

    next_order_id_ = nextOrderId;
    last_execution_timestamp_ = lastExecutionTimestamp;
    last_global_target_.clear(); // Derivable target; next execution will rebuild it.
}
