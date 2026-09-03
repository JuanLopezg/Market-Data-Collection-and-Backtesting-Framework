#include "trading_engine.h"

#include <algorithm>
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
      trade_recorder_(tradeRecorder),
      decision_engine_(strategies, indicators),
      execution_engine_(strategies, account, tradeRecorder, exchange)
{}


StrategyInstance& TradingEngine::strategyById(StrategyID strategyId)
{
    for (auto& strategy : strategies_) {
        if (strategy.id() == strategyId)
            return strategy;
    }

    throw std::out_of_range("Strategy id not found");
}


void TradingEngine::syncStrategyPositionMirrors()
{
    for (auto& strategy : strategies_)
        strategy.virtualPositions() = execution_engine_.strategyPosition(strategy.id());
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

    const std::unordered_map<Coin, double> empty;
    StrategyPositionSnapshot restoredStrategyPositions;

    for (auto& strategy : strategies_) {
        const auto it = strategyStates.find(strategy.id());
        if (it == strategyStates.end()) {
            // SQLite v1 may omit a completely empty strategy because it has no value rows.
            strategy.restoreState(empty, empty, empty);
            restoredStrategyPositions.emplace(strategy.id(), VirtualPositionState{});
            continue;
        }

        strategy.restoreState(
            it->second->signals,
            it->second->desired_weights,
            empty
        );

        VirtualPositionState restoredPositions;
        for (const auto& [coin, quantity] : it->second->virtual_positions)
            restoredPositions.set(coin, quantity);

        restoredStrategyPositions.emplace(strategy.id(), std::move(restoredPositions));
    }

    DecisionBatch pendingDecisions;
    pendingDecisions.decision_timestamp = snapshot.last_bar_close_timestamp;
    std::unordered_set<StrategyID> restoredPendingStrategyIds;

    for (const PendingPlanSnapshot& pending : snapshot.pending_plans) {
        (void)strategyById(pending.strategy_id);
        if (!restoredPendingStrategyIds.insert(pending.strategy_id).second)
            throw std::invalid_argument("Duplicate restored pending strategy plan");

        StrategyDecisionIntent intent;
        intent.strategy_id = pending.strategy_id;
        intent.decision_timestamp = pending.plan.timestamp();
        intent.reference_capital = pending.plan.referenceCapital();
        intent.decisions = pending.plan.values();
        pendingDecisions.strategies.push_back(std::move(intent));
    }

    for (const TrackedOrder& order : snapshot.orders)
        (void)strategyById(order.request.strategy_id);

    decision_engine_.restoreState(
        pendingDecisions,
        snapshot.last_bar_close_timestamp
    );

    execution_engine_.restoreState(
        snapshot.account_cash,
        snapshot.account_positions,
        restoredStrategyPositions,
        snapshot.orders,
        snapshot.processed_fill_ids,
        snapshot.next_order_id,
        snapshot.last_execution_timestamp
    );

    syncStrategyPositionMirrors();
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
    snapshot.last_bar_close_timestamp = decision_engine_.lastBarCloseTimestamp();
    snapshot.last_execution_timestamp = execution_engine_.lastExecutionTimestamp();
    snapshot.next_order_id = execution_engine_.nextOrderId();
    snapshot.account_cash = execution_engine_.account().cash();
    snapshot.account_positions = execution_engine_.account().positions().values();
    snapshot.processed_fill_ids = execution_engine_.orderManager().processedFillIds();

    snapshot.strategies.reserve(strategies_.size());
    for (const auto& strategy : strategies_) {
        StrategyStateSnapshot strategyState;
        strategyState.strategy_id = strategy.id();
        strategyState.signals = strategy.signalState().values();
        strategyState.desired_weights = strategy.desiredWeights().values();
        strategyState.virtual_positions = execution_engine_.strategyPosition(strategy.id()).values();
        snapshot.strategies.push_back(std::move(strategyState));
    }

    const DecisionBatch& pendingDecisions = decision_engine_.pendingDecisions();
    snapshot.pending_plans.reserve(pendingDecisions.strategies.size());
    for (const StrategyDecisionIntent& intent : pendingDecisions.strategies) {
        RebalancePlan plan(intent.decision_timestamp, intent.reference_capital);
        for (const auto& [coin, decision] : intent.decisions)
            plan.set(coin, decision);

        snapshot.pending_plans.push_back({intent.strategy_id, std::move(plan)});
    }

    snapshot.orders.reserve(execution_engine_.orderManager().orders().size());
    for (const auto& [orderId, order] : execution_engine_.orderManager().orders()) {
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
 * Purpose : Update strategy decision state at one completed market-data slice
 **************************************************************************************/
void TradingEngine::onBarClose(
    const MarketData& marketData,
    Timestamp ts,
    const PriceSnapshot& marks
)
{
    requireTradingEnabled();

    decision_engine_.onBarClose(
        marketData,
        ts,
        execution_engine_.accountEquity(marks),
        execution_engine_.strategyPositions()
    );

    persist();
}


/**************************************************************************************
 * Purpose : Resolve pending decision intent at executable prices
 **************************************************************************************/
void TradingEngine::executePendingPlans(
    Timestamp ts,
    const ExecutionReferencePrices& prices
)
{
    requireTradingEnabled();

    const bool hadPendingPlans = decision_engine_.hasPendingDecisions();

    execution_engine_.executeDecisionBatch(
        ts,
        prices,
        decision_engine_.pendingDecisions(),
        [this](const std::optional<Fill>& fill) { persist(fill); }
    );

    if (hadPendingPlans) {
        decision_engine_.clearPendingDecisions();
        persist();
    }
}


/**************************************************************************************
 * Purpose : Route queued asynchronous exchange lifecycle events
 **************************************************************************************/
void TradingEngine::processExchangeEvents()
{
    execution_engine_.processExchangeEvents(
        [this](const std::optional<Fill>& fill) { persist(fill); }
    );

    syncStrategyPositionMirrors();
}
