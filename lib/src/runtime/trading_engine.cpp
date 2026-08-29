#include "trading_engine.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>


TradingEngine::TradingEngine(
    StrategyPortfolio& strategies,
    Account& account,
    TradeRecorder& tradeRecorder,
    const IndicatorEngine& indicators,
    Exchange& exchange
)
    : strategies_(strategies),
      account_(account),
      trade_recorder_(tradeRecorder),
      indicators_(indicators),
      exchange_(exchange)
{}


StrategyInstance& TradingEngine::strategyById(StrategyID strategyId)
{
    for (auto& strategy : strategies_) {
        if (strategy.id() == strategyId)
            return strategy;
    }

    throw std::out_of_range("Strategy id not found");
}


void TradingEngine::requireTradingEnabled() const
{
    if (!trading_enabled_)
        throw std::logic_error("TradingEngine is paused pending recovery/reconciliation");
}


void TradingEngine::attachStateStore(StateStore& stateStore)
{
    // Do not write here: a live runtime must be able to inspect/recover an existing DB
    // before any fresh in-memory state has a chance to overwrite it.
    state_store_ = &stateStore;
}


void TradingEngine::checkpoint() const
{
    persist();
}


/**************************************************************************************
 * Purpose : Restore persisted operational state without submitting/canceling any orders
 **************************************************************************************/
void TradingEngine::restoreState(const TradingStateSnapshot& snapshot)
{
    if (snapshot.schema_version != TradingStateSnapshot::CURRENT_SCHEMA_VERSION)
        throw std::invalid_argument("Unsupported trading state schema version");
    if (snapshot.next_order_id == 0)
        throw std::invalid_argument("Restored next order id must be non-zero");

    std::unordered_map<StrategyID, const StrategyStateSnapshot*> strategyStates;
    for (const StrategyStateSnapshot& state : snapshot.strategies) {
        if (!strategyStates.emplace(state.strategy_id, &state).second)
            throw std::invalid_argument("Duplicate restored strategy state");
        (void)strategyById(state.strategy_id); // Reject state for strategies not configured now.
    }

    account_.restoreState(snapshot.account_cash, snapshot.account_positions);

    const std::unordered_map<Coin, double> empty;
    for (auto& strategy : strategies_) {
        const auto it = strategyStates.find(strategy.id());
        if (it == strategyStates.end()) {
            // SQLite v1 may omit a completely empty strategy because it has no value rows.
            strategy.restoreState(empty, empty, empty);
            continue;
        }

        strategy.restoreState(
            it->second->signals,
            it->second->desired_weights,
            it->second->virtual_positions
        );
    }

    pending_plans_.clear();
    for (const PendingPlanSnapshot& pending : snapshot.pending_plans) {
        (void)strategyById(pending.strategy_id);
        if (!pending_plans_.emplace(pending.strategy_id, pending.plan).second)
            throw std::invalid_argument("Duplicate restored pending strategy plan");
    }

    for (const TrackedOrder& order : snapshot.orders)
        (void)strategyById(order.request.strategy_id);
    order_manager_.restore(snapshot.orders, snapshot.processed_fill_ids);

    OrderID maximumOrderId = 0;
    for (const TrackedOrder& order : snapshot.orders)
        maximumOrderId = std::max(maximumOrderId, order.request.order_id);
    if (snapshot.next_order_id <= maximumOrderId)
        throw std::invalid_argument("Restored next order id does not follow tracked orders");

    next_order_id_ = snapshot.next_order_id;
    last_bar_close_timestamp_ = snapshot.last_bar_close_timestamp;
    last_execution_timestamp_ = snapshot.last_execution_timestamp;
    last_global_target_.clear(); // Derivable target; next decision/execution will rebuild it.
}


/**************************************************************************************
 * Purpose : Rebuild non-operational TradeRecorder analytics from the persisted fill log
 **************************************************************************************/
void TradingEngine::rebuildTradeRecorder(const std::vector<Fill>& fills)
{
    std::vector<Fill> ordered = fills;
    std::sort(ordered.begin(), ordered.end(), [](const Fill& a, const Fill& b) {
        if (a.timestamp != b.timestamp)
            return a.timestamp < b.timestamp;
        return a.fill_id < b.fill_id;
    });

    trade_recorder_.clear();
    std::unordered_set<FillID> seen;
    for (const Fill& fill : ordered) {
        fill.validate();
        if (fill.fill_id == 0 || !seen.insert(fill.fill_id).second)
            throw std::invalid_argument("Invalid or duplicate persisted fill during analytics rebuild");

        const StrategyInstance& strategy = strategyById(fill.strategy_id);
        trade_recorder_.onFill(fill, strategy.name());
    }
}


/**************************************************************************************
 * Purpose : Create a plain-data operational snapshot suitable for persistence/recovery
 **************************************************************************************/
TradingStateSnapshot TradingEngine::stateSnapshot() const
{
    TradingStateSnapshot snapshot;
    snapshot.last_bar_close_timestamp = last_bar_close_timestamp_;
    snapshot.last_execution_timestamp = last_execution_timestamp_;
    snapshot.next_order_id = next_order_id_;
    snapshot.account_cash = account_.cash();
    snapshot.account_positions = account_.positions().values();
    snapshot.processed_fill_ids = order_manager_.processedFillIds();

    snapshot.strategies.reserve(strategies_.size());
    for (const auto& strategy : strategies_) {
        StrategyStateSnapshot strategyState;
        strategyState.strategy_id = strategy.id();
        strategyState.signals = strategy.signalState().values();
        strategyState.desired_weights = strategy.desiredWeights().values();
        strategyState.virtual_positions = strategy.virtualPositions().values();
        snapshot.strategies.push_back(std::move(strategyState));
    }

    snapshot.pending_plans.reserve(pending_plans_.size());
    for (const auto& [strategyId, plan] : pending_plans_)
        snapshot.pending_plans.push_back({strategyId, plan});

    snapshot.orders.reserve(order_manager_.orders().size());
    for (const auto& [orderId, order] : order_manager_.orders()) {
        (void)orderId;
        snapshot.orders.push_back(order);
    }

    return snapshot;
}


void TradingEngine::persist(const std::optional<Fill>& newFill) const
{
    if (state_store_)
        state_store_->save(stateSnapshot(), newFill);
}


/**************************************************************************************
 * Purpose : Apply one actual fill to lifecycle/account/strategy/analytics state
 **************************************************************************************/
void TradingEngine::applyFill(const Fill& fill)
{
    // Validate/idempotently record the fill before mutating account state.
    if (!order_manager_.onFill(fill))
        return; // A replayed FillID must not mutate account/strategy/analytics twice.

    account_.applyFill(fill);

    StrategyInstance& strategy = strategyById(fill.strategy_id);
    strategy.applyVirtualFill(fill);
    trade_recorder_.onFill(fill, strategy.name());

    // Fill audit row + resulting operational snapshot commit atomically.
    persist(fill);
}


/**************************************************************************************
 * Purpose : Route queued exchange events exactly as a future live runtime will
 **************************************************************************************/
void TradingEngine::processExchangeEvents()
{
    const std::vector<ExchangeEvent> events = exchange_.drainEvents();

    for (const ExchangeEvent& event : events) {
        if (const auto* update = std::get_if<OrderUpdate>(&event)) {
            order_manager_.onOrderUpdate(*update);
            persist();
            continue;
        }

        if (const auto* fill = std::get_if<Fill>(&event))
            applyFill(*fill);
    }
}


/**************************************************************************************
 * Purpose : Update signals at a completed bar and create strategy rebalance plans
 **************************************************************************************/
void TradingEngine::onBarClose(
    const MarketData& marketData,
    Timestamp ts,
    const PriceSnapshot& marks
)
{
    requireTradingEnabled();
    const double accountEquity = account_.equity(marks);

    for (auto& strategy : strategies_) {
        strategy.updateSignals(marketData, ts, indicators_);

        const double strategyCapital = accountEquity * strategy.allocationWeight();
        const auto plan = strategy.calculateRebalancePlan(
            marketData,
            ts,
            strategyCapital
        );

        if (plan && plan->size() > 0)
            pending_plans_.insert_or_assign(strategy.id(), *plan);
    }

    last_bar_close_timestamp_ = ts;
    persist();
}


/**************************************************************************************
 * Purpose : Resolve pending targets and submit/cancel orders at executable prices
 **************************************************************************************/
void TradingEngine::executePendingPlans(
    Timestamp ts,
    const ExecutionReferencePrices& prices
)
{
    requireTradingEnabled();
    last_execution_timestamp_ = ts;

    if (pending_plans_.empty()) {
        persist();
        return;
    }

    std::vector<TargetPortfolio> monetaryTargets;
    std::vector<StrategyExecutionTarget> quantityTargets;
    std::vector<VirtualPositionState> currentStrategyPositions;

    monetaryTargets.reserve(strategies_.size());
    quantityTargets.reserve(strategies_.size());
    currentStrategyPositions.reserve(strategies_.size());

    for (const auto& strategy : strategies_) {
        TargetPortfolio monetaryTarget;

        const auto planIt = pending_plans_.find(strategy.id());
        if (planIt != pending_plans_.end()) {
            monetaryTarget = strategy_target_resolver_.resolve(
                planIt->second,
                strategy.virtualPositions(),
                prices
            );
        } else {
            // No new decision means preserve the already-filled strategy quantities.
            for (const auto& [coin, quantity] : strategy.virtualPositions().values()) {
                if (!prices.contains(coin))
                    throw std::runtime_error("Missing execution price for held strategy asset");

                monetaryTarget.set(coin, quantity * prices.get(coin));
            }
        }

        monetaryTargets.push_back(monetaryTarget);
        quantityTargets.push_back({
            strategy.id(),
            account_target_resolver_.resolve(monetaryTarget, prices)
        });
        currentStrategyPositions.push_back(strategy.virtualPositions());
    }

    last_global_target_ = portfolio_aggregator_.aggregate(monetaryTargets);

    const ExecutionPlan executionPlan = execution_coordinator_.createMarketPlan(
        quantityTargets,
        currentStrategyPositions,
        order_manager_,
        ts,
        ts,
        next_order_id_
    );

    // Persist cancel intent BEFORE the external side effect. A restart can then reconcile it.
    for (const OrderID orderId : executionPlan.order_ids_to_cancel) {
        order_manager_.markCancelRequested(orderId, ts);
        persist();
        exchange_.cancelOrder(orderId);
    }

    // Persist the locally-created order BEFORE submission. If submit succeeds and the
    // process dies immediately afterwards, reconciliation can still identify the intent.
    for (const ExecutionOrder& order : executionPlan.orders_to_submit) {
        order_manager_.track(order);
        persist();

        exchange_.submitOrder(order);
        order_manager_.markSubmitted(order.order_id, ts);
        persist();
    }

    pending_plans_.clear();
    persist();
}
