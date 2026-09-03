#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <chrono>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "account.h"
#include "contract_json_codec.h"
#include "decision_batch.h"
#include "exchange_snapshot_event.h"
#include "exchange_snapshot_request.h"
#include "execution_engine.h"
#include "execution_cycle_complete.h"
#include "execution_planning_state.h"
#include "execution_price_snapshot.h"
#include "market_slice_snapshot.h"
#include "message_bus_exchange.h"
#include "nats_jetstream_message_bus.h"
#include "order_planning.h"
#include "postgres_state_store.h"
#include "reconciler.h"
#include "service_logging.h"
#include "trade_recorder.h"
#include "trading_state_snapshot.h"
#include "transport_subjects.h"


namespace {

std::atomic<bool> running{true};

void stopHandler(int)
{
    running.store(false);
}


struct Options {
    std::string nats_url = "nats://127.0.0.1:4222";
    std::string postgres_connection =
        "host=127.0.0.1 port=5432 dbname=algotrading user=algotrading password=algotrading";
    std::string stream = "ALGOTRADING_RUNTIME";
    std::string exchange_control_stream = "ALGOTRADING_EXCHANGE_CONTROL";
    double initial_cash = 100000.0;
    int poll_timeout_ms = 250;
    std::vector<ExecutionStrategyDescriptor> strategies;
};


ExecutionStrategyDescriptor parseStrategy(const std::string& value)
{
    const std::size_t separator = value.find(':');
    if (separator == std::string::npos || separator == 0 || separator + 1 >= value.size())
        throw std::invalid_argument("--strategy must use ID:NAME");

    const unsigned long parsedId = std::stoul(value.substr(0, separator));
    if (parsedId > static_cast<unsigned long>(std::numeric_limits<StrategyID>::max()))
        throw std::invalid_argument("Strategy id is out of range");

    ExecutionStrategyDescriptor descriptor;
    descriptor.strategy_id = static_cast<StrategyID>(parsedId);
    descriptor.name = value.substr(separator + 1);
    return descriptor;
}


Options parseOptions(int argc, char** argv)
{
    Options options;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto requireValue = [&](const char* option) -> std::string {
            if (i + 1 >= argc)
                throw std::invalid_argument(std::string("Missing value for ") + option);
            return argv[++i];
        };

        if (arg == "--nats-url")
            options.nats_url = requireValue("--nats-url");
        else if (arg == "--postgres")
            options.postgres_connection = requireValue("--postgres");
        else if (arg == "--stream")
            options.stream = requireValue("--stream");
        else if (arg == "--exchange-control-stream")
            options.exchange_control_stream = requireValue("--exchange-control-stream");
        else if (arg == "--initial-cash")
            options.initial_cash = std::stod(requireValue("--initial-cash"));
        else if (arg == "--poll-timeout-ms")
            options.poll_timeout_ms = std::stoi(requireValue("--poll-timeout-ms"));
        else if (arg == "--strategy")
            options.strategies.push_back(parseStrategy(requireValue("--strategy")));
        else if (arg == "--help") {
            std::cout
                << "Usage: algotrading_execution_state_service [options]\n"
                << "  --nats-url URL\n"
                << "  --postgres CONNECTION_STRING\n"
                << "  --stream NAME\n"
                << "  --exchange-control-stream NAME\n"
                << "  --initial-cash VALUE\n"
                << "  --poll-timeout-ms VALUE\n"
                << "  --strategy ID:NAME   (repeat for each configured strategy)\n";
            std::exit(0);
        }
        else
            throw std::invalid_argument("Unknown option: " + arg);
    }

    if (options.strategies.empty())
        throw std::invalid_argument("At least one --strategy ID:NAME is required");
    if (options.stream.empty() || options.exchange_control_stream.empty())
        throw std::invalid_argument("Execution-state stream names cannot be empty");
    if (options.initial_cash <= 0.0)
        throw std::invalid_argument("--initial-cash must be positive");
    if (options.poll_timeout_ms <= 0)
        throw std::invalid_argument("--poll-timeout-ms must be positive");

    std::unordered_set<StrategyID> ids;
    for (const auto& strategy : options.strategies) {
        if (!ids.insert(strategy.strategy_id).second)
            throw std::invalid_argument("Duplicate --strategy id");
    }

    return options;
}


class ExecutionStateServiceRuntime {
private:
    const Options options_;
    std::vector<StrategyID> strategy_ids_;
    std::unordered_map<StrategyID, std::string> strategy_names_;

    NatsJetStreamMessageBus bus_;
    PostgresStateStore store_;
    Account account_;
    TradeRecorder trade_recorder_;
    MessageBusExchange exchange_;
    ExecutionEngine engine_;
    Reconciler reconciler_;

    struct ActiveExecutionCycle {
        Timestamp decision_timestamp = 0;
        Timestamp execution_timestamp = 0;
        std::string correlation_id;
    };

    std::optional<DecisionBatch> pending_decision_;
    std::optional<ActiveExecutionCycle> active_execution_cycle_;
    Timestamp last_decision_timestamp_ = 0;
    bool reconciled_ = false;

    DurableMessageBus::SubscriptionID exchange_snapshot_subscription_ = 0;
    DurableMessageBus::SubscriptionID exchange_event_subscription_ = 0;
    DurableMessageBus::SubscriptionID plan_subscription_ = 0;
    DurableMessageBus::SubscriptionID decision_subscription_ = 0;
    DurableMessageBus::SubscriptionID prices_subscription_ = 0;
    DurableMessageBus::SubscriptionID market_slice_subscription_ = 0;

    TradingStateSnapshot snapshot() const
    {
        TradingStateSnapshot result;
        result.last_bar_close_timestamp = last_decision_timestamp_;
        result.last_execution_timestamp = engine_.lastExecutionTimestamp();
        result.next_order_id = engine_.nextOrderId();
        result.account_cash = engine_.account().cash();
        result.account_positions = engine_.account().positions().values();
        result.processed_fill_ids = engine_.orderManager().processedFillIds();

        result.strategies.reserve(options_.strategies.size());
        for (const auto& descriptor : options_.strategies) {
            StrategyStateSnapshot strategy;
            strategy.strategy_id = descriptor.strategy_id;
            strategy.virtual_positions = engine_.strategyPosition(descriptor.strategy_id).values();
            result.strategies.push_back(std::move(strategy));
        }

        if (pending_decision_) {
            result.pending_plans.reserve(pending_decision_->strategies.size());
            for (const StrategyDecisionIntent& intent : pending_decision_->strategies) {
                RebalancePlan plan(intent.decision_timestamp, intent.reference_capital);
                for (const auto& [coin, decision] : intent.decisions)
                    plan.set(coin, decision);
                result.pending_plans.push_back({intent.strategy_id, std::move(plan)});
            }
        }

        result.orders.reserve(engine_.orderManager().orders().size());
        for (const auto& [orderId, order] : engine_.orderManager().orders()) {
            (void)orderId;
            result.orders.push_back(order);
        }
        return result;
    }

    void persist(const std::optional<Fill>& fill = std::nullopt)
    {
        store_.save(snapshot(), fill);
        LG_DEBUG(
            "service=execution-state event=state_persisted fill_id={} last_decision_timestamp={} last_execution_timestamp={} cash={} tracked_orders={}",
            fill ? std::to_string(fill->fill_id) : std::string{"none"},
            last_decision_timestamp_,
            engine_.lastExecutionTimestamp(),
            engine_.account().cash(),
            engine_.orderManager().orders().size()
        );
    }

    void publishAccountSnapshot(
        Timestamp timestamp,
        const std::string& messageId,
        const std::string& correlationId = {}
    )
    {
        AccountSnapshot value;
        value.metadata.schema_version = 1;
        value.metadata.message_id = messageId;
        value.metadata.correlation_id = correlationId;
        value.metadata.produced_at = timestamp;
        value.timestamp = timestamp;
        value.cash = engine_.account().cash();
        value.positions = engine_.account().positions().values();

        for (const auto& strategy : options_.strategies) {
            value.strategy_positions.emplace(
                strategy.strategy_id,
                engine_.strategyPosition(strategy.strategy_id).values()
            );
        }

        bus_.publish(
            TransportSubjects::ACCOUNT_SNAPSHOT,
            ContractJsonCodec::encode(value),
            value.metadata.message_id
        );
        LG_INFO(
            "service=execution-state event=account_snapshot_published timestamp={} cash={} positions={} strategy_position_sets={} message_id={}",
            value.timestamp,
            value.cash,
            value.positions.size(),
            value.strategy_positions.size(),
            value.metadata.message_id
        );
    }

    void maybePublishExecutionCycleComplete()
    {
        if (!active_execution_cycle_)
            return;

        // A terminal Filled OrderUpdate may be delivered by its own durable consumer
        // before the corresponding Fill consumer has applied cash/position state.
        // Therefore "no open orders" alone is not a safe barrier: every Filled order
        // must also have its complete fill quantity recorded locally.
        for (const auto& [orderId, order] : engine_.orderManager().orders()) {
            (void)orderId;
            if (order.isOpen())
                return;
            if (order.status == ExecutionOrderStatus::Filled &&
                order.remainingQuantity() > 1e-12)
                return;
        }

        const ActiveExecutionCycle cycle = *active_execution_cycle_;
        ExecutionCycleComplete value;
        value.metadata.schema_version = 1;
        value.metadata.message_id =
            "execution-cycle-complete:" + std::to_string(cycle.decision_timestamp) + ":" +
            std::to_string(cycle.execution_timestamp);
        value.metadata.correlation_id = cycle.correlation_id;
        value.metadata.produced_at = cycle.execution_timestamp;
        value.decision_timestamp = cycle.decision_timestamp;
        value.execution_timestamp = cycle.execution_timestamp;
        value.state_revision = planningState().state_revision;

        bus_.publish(
            TransportSubjects::EXECUTION_CYCLE_COMPLETE,
            ContractJsonCodec::encode(value),
            value.metadata.message_id
        );
        LG_INFO(
            "service=execution-state event=execution_cycle_complete decision_timestamp={} execution_timestamp={} state_revision={} message_id={}",
            value.decision_timestamp,
            value.execution_timestamp,
            value.state_revision,
            value.metadata.message_id
        );
        active_execution_cycle_.reset();
    }

    void restore(const TradingStateSnapshot& stored)
    {
        if (stored.schema_version != TradingStateSnapshot::CURRENT_SCHEMA_VERSION)
            throw std::invalid_argument("Unsupported execution-state schema version");

        StrategyPositionSnapshot positions;
        for (const StrategyStateSnapshot& strategy : stored.strategies) {
            if (!strategy_names_.contains(strategy.strategy_id))
                throw std::invalid_argument("Stored state contains unknown strategy id");

            VirtualPositionState restored;
            for (const auto& [coin, quantity] : strategy.virtual_positions)
                restored.set(coin, quantity);
            positions.emplace(strategy.strategy_id, std::move(restored));
        }

        engine_.restoreState(
            stored.account_cash,
            stored.account_positions,
            positions,
            stored.orders,
            stored.processed_fill_ids,
            stored.next_order_id,
            stored.last_execution_timestamp
        );

        last_decision_timestamp_ = stored.last_bar_close_timestamp;
        if (stored.last_execution_timestamp > stored.last_bar_close_timestamp) {
            active_execution_cycle_ = ActiveExecutionCycle{
                stored.last_bar_close_timestamp,
                stored.last_execution_timestamp,
                "execution-state-recovery"
            };
        }

        if (!stored.pending_plans.empty()) {
            DecisionBatch restoredBatch;
            restoredBatch.decision_timestamp = stored.pending_plans.front().plan.timestamp();

            for (const PendingPlanSnapshot& pending : stored.pending_plans) {
                if (pending.plan.timestamp() != restoredBatch.decision_timestamp)
                    throw std::invalid_argument(
                        "Execution-state contains multiple pending decision timestamps"
                    );

                StrategyDecisionIntent intent;
                intent.strategy_id = pending.strategy_id;
                intent.decision_timestamp = pending.plan.timestamp();
                intent.reference_capital = pending.plan.referenceCapital();
                intent.decisions = pending.plan.values();
                restoredBatch.strategies.push_back(std::move(intent));
            }

            // A plan that already reached a later execution timestamp was durably applied.
            if (stored.last_execution_timestamp <= restoredBatch.decision_timestamp)
                pending_decision_ = std::move(restoredBatch);
        }

        std::vector<Fill> fills = store_.loadFills();
        std::sort(fills.begin(), fills.end(), [](const Fill& a, const Fill& b) {
            if (a.timestamp != b.timestamp)
                return a.timestamp < b.timestamp;
            return a.fill_id < b.fill_id;
        });

        std::unordered_set<FillID> seen;
        for (const Fill& fill : fills) {
            if (!seen.insert(fill.fill_id).second)
                throw std::invalid_argument("Duplicate fill in PostgreSQL audit trail");
            const auto name = strategy_names_.find(fill.strategy_id);
            if (name == strategy_names_.end())
                throw std::invalid_argument("Persisted fill contains unknown strategy id");
            trade_recorder_.onFill(fill, name->second);
        }
    }

    bool validateDecisionStrategies(const DecisionBatch& batch) const
    {
        std::unordered_set<StrategyID> seen;
        for (const StrategyDecisionIntent& intent : batch.strategies) {
            if (!strategy_names_.contains(intent.strategy_id))
                return false;
            if (!seen.insert(intent.strategy_id).second)
                return false;
            if (intent.decision_timestamp != batch.decision_timestamp)
                return false;
        }
        return true;
    }

    ExecutionPlanningStateSnapshot planningState() const
    {
        return makeExecutionPlanningStateSnapshot(
            strategy_ids_,
            engine_.strategyPositions(),
            engine_.orderManager(),
            engine_.nextOrderId()
        );
    }

    static std::string planningRequestId(
        Timestamp decisionTimestamp,
        Timestamp executionTimestamp,
        std::uint64_t stateRevision
    )
    {
        return "order-plan-request:" + std::to_string(decisionTimestamp) + ":" +
            std::to_string(executionTimestamp) + ":" + std::to_string(stateRevision);
    }

    DecisionBatch decisionFor(Timestamp decisionTimestamp) const
    {
        if (pending_decision_) {
            if (pending_decision_->decision_timestamp != decisionTimestamp)
                throw std::logic_error("Pending decision timestamp does not match execution prices");
            return *pending_decision_;
        }

        if (decisionTimestamp > last_decision_timestamp_)
            throw std::logic_error("Execution prices arrived before the decision barrier");

        DecisionBatch empty;
        empty.decision_timestamp = decisionTimestamp;
        return empty;
    }

    void publishPlanningRequest(
        const DecisionBatch& decision,
        const ExecutionPriceSnapshot& prices
    )
    {
        OrderPlanningRequest request;
        request.decision_timestamp = decision.decision_timestamp;
        request.execution_timestamp = prices.timestamp;
        request.decisions = decision;
        request.prices = prices;
        request.state = planningState();
        request.metadata.schema_version = 1;
        request.metadata.message_id = planningRequestId(
            request.decision_timestamp,
            request.execution_timestamp,
            request.state.state_revision
        );
        request.metadata.correlation_id = prices.metadata.message_id;
        request.metadata.produced_at = prices.timestamp;

        bus_.publish(
            TransportSubjects::ORDER_PLANNING_REQUEST,
            ContractJsonCodec::encode(request),
            request.metadata.message_id
        );
        LG_INFO(
            "service=execution-state event=planning_request_published decision_timestamp={} execution_timestamp={} state_revision={} tracked_orders={} next_order_id={} message_id={}",
            request.decision_timestamp,
            request.execution_timestamp,
            request.state.state_revision,
            request.state.orders.size(),
            request.state.next_order_id,
            request.metadata.message_id
        );
    }

    void requestExchangeSnapshot()
    {
        ExchangeSnapshotRequest request;
        request.metadata.schema_version = 1;
        request.metadata.correlation_id = "execution-state-startup";
        request.metadata.produced_at = engine_.lastExecutionTimestamp();

        const auto nonce = std::chrono::system_clock::now().time_since_epoch().count();
        request.metadata.message_id =
            "exchange-snapshot-request:" + std::to_string(nonce);

        bus_.publish(
            TransportSubjects::EXCHANGE_SNAPSHOT_REQUEST,
            ContractJsonCodec::encode(request),
            request.metadata.message_id
        );
        LG_INFO(
            "service=execution-state event=exchange_snapshot_requested last_execution_timestamp={} message_id={}",
            engine_.lastExecutionTimestamp(),
            request.metadata.message_id
        );
    }

    void recoverOutboundIntents()
    {
        // Created orders were durably staged before submit publication. Re-publishing the
        // same OrderID is intentionally idempotent at the transport/gateway boundary.
        std::vector<OrderID> created;
        std::vector<OrderID> cancels;

        for (const auto& [orderId, tracked] : engine_.orderManager().orders()) {
            if (tracked.status == ExecutionOrderStatus::Created)
                created.push_back(orderId);
            if (tracked.cancel_requested && tracked.isOpen())
                cancels.push_back(orderId);
        }
        std::sort(created.begin(), created.end());
        std::sort(cancels.begin(), cancels.end());

        for (const OrderID orderId : created) {
            const TrackedOrder* tracked = engine_.orderManager().find(orderId);
            if (!tracked)
                throw std::logic_error("Created recovery order disappeared");
            exchange_.submitOrder(tracked->request);
            engine_.orderManager().markSubmitted(orderId, tracked->request.created_at);
            persist();
        }

        for (const OrderID orderId : cancels)
            exchange_.cancelOrder(orderId);
    }

    DurableMessageDisposition onExchangeSnapshot(const BusMessage& message)
    {
        try {
            const ExchangeSnapshotEvent value =
                ContractJsonCodec::decodeExchangeSnapshotEvent(message.payload);
            if (value.snapshot.timestamp == 0)
                return DurableMessageDisposition::Terminate;

            const ReconciliationReport report = reconciler_.compare(snapshot(), value.snapshot);
            if (!report.clean()) {
                reconciled_ = false;
                LG_ALERT(
                    "service=execution-state event=reconciliation_blocked exchange_timestamp={} issues={} trading_state=paused",
                    value.snapshot.timestamp,
                    report.issues.size()
                );
                for (const ReconciliationIssue& issue : report.issues) {
                    LG_WARN(
                        "service=execution-state event=reconciliation_issue kind={} coin={} order_id={} local_value={} exchange_value={} message={}",
                        static_cast<int>(issue.kind),
                        issue.coin,
                        issue.order_id,
                        issue.local_value,
                        issue.exchange_value,
                        issue.message
                    );
                }
                return DurableMessageDisposition::Ack;
            }

            reconciled_ = true;
            LG_INFO(
                "service=execution-state event=reconciliation_clean exchange_timestamp={} cash={} positions={} open_orders={}",
                value.snapshot.timestamp,
                value.snapshot.cash,
                value.snapshot.positions.size(),
                value.snapshot.open_orders.size()
            );
            recoverOutboundIntents();
            publishAccountSnapshot(
                value.snapshot.timestamp,
                "account-snapshot:reconciled:" + std::to_string(value.snapshot.timestamp),
                value.metadata.message_id
            );
            maybePublishExecutionCycleComplete();
            return DurableMessageDisposition::Ack;
        }
        catch (const std::exception& error) {
            LG_ERROR("service=execution-state event=exchange_snapshot_failed disposition=retry error={}", error.what());
            return DurableMessageDisposition::Retry;
        }
    }

    DurableMessageDisposition onMarketSlice(const BusMessage& message)
    {
        try {
            if (!reconciled_)
                return DurableMessageDisposition::Retry;

            const MarketSliceSnapshot slice =
                ContractJsonCodec::decodeMarketSliceSnapshot(message.payload);
            if (slice.timestamp == 0 || slice.bars.empty())
                return DurableMessageDisposition::Terminate;

            // The replay controller never releases the next close while the previous
            // execution cycle is active. Keep this guard in the state authority too so
            // a broken controller cannot manufacture a close snapshot from stale state.
            if (active_execution_cycle_ &&
                active_execution_cycle_->execution_timestamp >= slice.timestamp)
                return DurableMessageDisposition::Retry;

            publishAccountSnapshot(
                slice.timestamp,
                "account-snapshot:close:" + std::to_string(slice.timestamp),
                slice.metadata.message_id
            );
            return DurableMessageDisposition::Ack;
        }
        catch (const std::exception& error) {
            LG_ERROR("service=execution-state event=market_slice_failed disposition=retry error={}", error.what());
            return DurableMessageDisposition::Retry;
        }
    }

    DurableMessageDisposition onDecision(const BusMessage& message)
    {
        try {
            const DecisionBatch batch = ContractJsonCodec::decodeDecisionBatch(message.payload);
            if (batch.decision_timestamp == 0 || !validateDecisionStrategies(batch))
                return DurableMessageDisposition::Terminate;

            if (batch.decision_timestamp <= last_decision_timestamp_)
                return DurableMessageDisposition::Ack;
            if (pending_decision_)
                return DurableMessageDisposition::Retry;

            last_decision_timestamp_ = batch.decision_timestamp;
            if (!batch.strategies.empty())
                pending_decision_ = batch;
            persist();
            LG_INFO(
                "service=execution-state event=decision_persisted timestamp={} strategies={} pending_decision={} message_id={}",
                batch.decision_timestamp,
                batch.strategies.size(),
                pending_decision_.has_value(),
                batch.metadata.message_id
            );
            return DurableMessageDisposition::Ack;
        }
        catch (const std::exception& error) {
            LG_ERROR("service=execution-state event=decision_failed disposition=retry error={}", error.what());
            return DurableMessageDisposition::Retry;
        }
    }

    DurableMessageDisposition onPrices(const BusMessage& message)
    {
        try {
            if (!reconciled_)
                return DurableMessageDisposition::Retry;

            const ExecutionPriceSnapshot value =
                ContractJsonCodec::decodeExecutionPriceSnapshot(message.payload);
            if (value.timestamp == 0 || value.decision_timestamp == 0 || value.prices.empty())
                return DurableMessageDisposition::Terminate;

            if (value.timestamp < engine_.lastExecutionTimestamp())
                return DurableMessageDisposition::Ack;
            if (value.timestamp == engine_.lastExecutionTimestamp()) {
                publishAccountSnapshot(
                    value.timestamp,
                    "account-snapshot:execution:" + std::to_string(value.timestamp),
                    value.metadata.message_id
                );
                return DurableMessageDisposition::Ack;
            }

            LG_INFO(
                "service=execution-state event=execution_prices_received decision_timestamp={} execution_timestamp={} prices={} message_id={}",
                value.decision_timestamp,
                value.timestamp,
                value.prices.size(),
                value.metadata.message_id
            );
            const DecisionBatch decision = decisionFor(value.decision_timestamp);
            publishPlanningRequest(decision, value);

            // The request itself is now durable and self-contained (decision + executable
            // prices + state snapshot). ACKing the price is therefore crash-safe.
            return DurableMessageDisposition::Ack;
        }
        catch (const std::logic_error&) {
            return DurableMessageDisposition::Retry;
        }
        catch (const std::exception& error) {
            LG_ERROR("service=execution-state event=execution_prices_failed disposition=retry error={}", error.what());
            return DurableMessageDisposition::Retry;
        }
    }

    DurableMessageDisposition onPlan(const BusMessage& message)
    {
        try {
            if (!reconciled_)
                return DurableMessageDisposition::Retry;

            const OrderPlanBatch plan = ContractJsonCodec::decodeOrderPlanBatch(message.payload);
            if (plan.decision_timestamp == 0 || plan.execution_timestamp == 0 ||
                plan.state_revision == 0 || plan.next_order_id == 0 ||
                plan.decisions.decision_timestamp != plan.decision_timestamp ||
                plan.prices.decision_timestamp != plan.decision_timestamp ||
                plan.prices.timestamp != plan.execution_timestamp ||
                plan.prices.prices.empty())
                return DurableMessageDisposition::Terminate;

            if (plan.execution_timestamp <= engine_.lastExecutionTimestamp())
                return DurableMessageDisposition::Ack;

            if (pending_decision_ &&
                pending_decision_->decision_timestamp != plan.decision_timestamp) {
                if (plan.decision_timestamp < pending_decision_->decision_timestamp)
                    return DurableMessageDisposition::Ack;
                return DurableMessageDisposition::Retry;
            }
            if (!pending_decision_) {
                if (plan.decision_timestamp < last_decision_timestamp_)
                    return DurableMessageDisposition::Ack;
                if (plan.decision_timestamp > last_decision_timestamp_)
                    return DurableMessageDisposition::Terminate;
            }

            const ExecutionPlanningStateSnapshot current = planningState();
            if (plan.state_revision != current.state_revision) {
                LG_WARN(
                    "service=execution-state event=stale_order_plan plan_revision={} current_revision={} decision_timestamp={} execution_timestamp={} action=replan",
                    plan.state_revision,
                    current.state_revision,
                    plan.decision_timestamp,
                    plan.execution_timestamp
                );
                // State changed while planning (for example a fill/order update arrived).
                // The plan echoes its decision/price context, so publish a new durable request
                // against current state BEFORE ACKing this stale proposal.
                publishPlanningRequest(plan.decisions, plan.prices);
                return DurableMessageDisposition::Ack;
            }

            const std::string expectedRequest = planningRequestId(
                plan.decision_timestamp,
                plan.execution_timestamp,
                current.state_revision
            );
            if (plan.metadata.correlation_id != expectedRequest)
                return DurableMessageDisposition::Terminate;

            (void)decisionFor(plan.decision_timestamp); // validates decision barrier/correlation

            engine_.applyOrderPlan(
                plan,
                [this](const std::optional<Fill>& fill) { persist(fill); }
            );
            active_execution_cycle_ = ActiveExecutionCycle{
                plan.decision_timestamp,
                plan.execution_timestamp,
                plan.metadata.message_id
            };

            if (pending_decision_ &&
                pending_decision_->decision_timestamp == plan.decision_timestamp)
                pending_decision_.reset();
            persist();
            LG_INFO(
                "service=execution-state event=order_plan_applied decision_timestamp={} execution_timestamp={} state_revision={} cancels={} submits={} tracked_orders={} next_order_id={}",
                plan.decision_timestamp,
                plan.execution_timestamp,
                plan.state_revision,
                plan.cancel_order_ids.size(),
                plan.submit_orders.size(),
                engine_.orderManager().orders().size(),
                engine_.nextOrderId()
            );

            publishAccountSnapshot(
                plan.execution_timestamp,
                "account-snapshot:execution:" + std::to_string(plan.execution_timestamp),
                plan.metadata.message_id
            );
            maybePublishExecutionCycleComplete();
            return DurableMessageDisposition::Ack;
        }
        catch (const std::exception& error) {
            LG_ERROR("service=execution-state event=order_plan_failed disposition=retry error={}", error.what());
            return DurableMessageDisposition::Retry;
        }
    }

    DurableMessageDisposition onOrderUpdate(const BusMessage& message)
    {
        try {
            const OrderUpdateEvent value = ContractJsonCodec::decodeOrderUpdateEvent(message.payload);
            LG_INFO(
                "service=execution-state event=order_update_received order_id={} timestamp={} status={} exchange_order_id={} message={}",
                value.update.order_id,
                value.update.timestamp,
                static_cast<int>(value.update.status),
                value.update.exchange_order_id,
                value.update.message
            );
            engine_.processExchangeEvent(
                ExchangeEvent{value.update},
                [this](const std::optional<Fill>& fill) { persist(fill); }
            );
            maybePublishExecutionCycleComplete();
            return DurableMessageDisposition::Ack;
        }
        catch (const std::exception& error) {
            LG_ERROR("service=execution-state event=order_update_failed disposition=retry error={}", error.what());
            return DurableMessageDisposition::Retry;
        }
    }

    DurableMessageDisposition onFill(const BusMessage& message)
    {
        try {
            const FillEvent value = ContractJsonCodec::decodeFillEvent(message.payload);
            LG_INFO(
                "service=execution-state event=fill_received fill_id={} order_id={} strategy_id={} timestamp={} coin={} side={} quantity={} price={} commission={}",
                value.fill.fill_id,
                value.fill.order_id,
                value.fill.strategy_id,
                value.fill.timestamp,
                value.fill.coin,
                value.fill.side == OrderSide::Buy ? "buy" : "sell",
                value.fill.quantity,
                value.fill.price,
                value.fill.commission
            );
            engine_.processExchangeEvent(
                ExchangeEvent{value.fill},
                [this](const std::optional<Fill>& fill) { persist(fill); }
            );
            publishAccountSnapshot(
                value.fill.timestamp,
                "account-snapshot:fill:" + std::to_string(value.fill.fill_id),
                value.metadata.message_id
            );
            LG_INFO(
                "service=execution-state event=fill_applied fill_id={} cash={} positions={} processed_fill_ids={}",
                value.fill.fill_id,
                engine_.account().cash(),
                engine_.account().positions().values().size(),
                engine_.orderManager().processedFillIds().size()
            );
            maybePublishExecutionCycleComplete();
            return DurableMessageDisposition::Ack;
        }
        catch (const std::exception& error) {
            LG_ERROR("service=execution-state event=fill_failed disposition=retry error={}", error.what());
            return DurableMessageDisposition::Retry;
        }
    }

    DurableMessageDisposition onExchangeEvent(const BusMessage& message)
    {
        if (message.subject == TransportSubjects::ORDER_UPDATE)
            return onOrderUpdate(message);
        if (message.subject == TransportSubjects::FILL)
            return onFill(message);
        return DurableMessageDisposition::Terminate;
    }

    DurableConsumerOptions consumer(const std::string& durable, const std::string& subject) const
    {
        DurableConsumerOptions result;
        result.stream = options_.stream;
        result.durable_name = durable;
        result.subject = subject;
        result.ack_wait_ms = 30000;
        result.max_deliver = 20;
        result.max_ack_pending = 256;
        return result;
    }

public:
    explicit ExecutionStateServiceRuntime(Options options)
        : options_(std::move(options)),
          bus_(options_.nats_url),
          store_(options_.postgres_connection),
          account_(options_.initial_cash),
          exchange_(bus_),
          engine_(options_.strategies, account_, trade_recorder_, exchange_)
    {
        for (const auto& strategy : options_.strategies) {
            strategy_ids_.push_back(strategy.strategy_id);
            strategy_names_.emplace(strategy.strategy_id, strategy.name);
        }

        bus_.ensureStream(options_.stream, TransportSubjects::runtimeSubjects());
        bus_.ensureStream(
            options_.exchange_control_stream,
            TransportSubjects::exchangeGatewayControlSubjects()
        );

        if (const auto stored = store_.load()) {
            LG_INFO("service=execution-state event=persisted_state_found action=restore");
            restore(*stored);
            LG_INFO(
                "service=execution-state event=state_restored last_decision_timestamp={} last_execution_timestamp={} cash={} positions={} tracked_orders={}",
                last_decision_timestamp_,
                engine_.lastExecutionTimestamp(),
                engine_.account().cash(),
                engine_.account().positions().values().size(),
                engine_.orderManager().orders().size()
            );
        }
        else {
            LG_INFO("service=execution-state event=persisted_state_missing action=initialize cash={}", options_.initial_cash);
            persist();
        }

        exchange_snapshot_subscription_ = bus_.subscribe(
            consumer("execution-state-exchange-snapshot", TransportSubjects::EXCHANGE_SNAPSHOT),
            [this](const BusMessage& message) { return onExchangeSnapshot(message); }
        );
        // One durable consumer owns the ordered public exchange event sequence.
        // Using separate OrderUpdate/Fill consumers can reorder Filled ahead of Fill
        // even when the gateway published Accepted -> Fill -> Filled correctly.
        exchange_event_subscription_ = bus_.subscribe(
            consumer("execution-state-exchange-events", TransportSubjects::EXECUTION_EVENT_FILTER),
            [this](const BusMessage& message) { return onExchangeEvent(message); }
        );
        plan_subscription_ = bus_.subscribe(
            consumer("execution-state-plans", TransportSubjects::ORDER_PLAN),
            [this](const BusMessage& message) { return onPlan(message); }
        );
        decision_subscription_ = bus_.subscribe(
            consumer("execution-state-decisions", TransportSubjects::DECISION_BATCH),
            [this](const BusMessage& message) { return onDecision(message); }
        );
        prices_subscription_ = bus_.subscribe(
            consumer("execution-state-prices", TransportSubjects::EXECUTION_PRICES),
            [this](const BusMessage& message) { return onPrices(message); }
        );
        market_slice_subscription_ = bus_.subscribe(
            consumer("execution-state-market-slices", TransportSubjects::MARKET_SLICE_SNAPSHOT),
            [this](const BusMessage& message) { return onMarketSlice(message); }
        );

        // Reconciliation is an explicit request/response boundary now. The request is
        // durable even if the gateway/simulated exchange starts later.
        requestExchangeSnapshot();
    }

    ~ExecutionStateServiceRuntime()
    {
        bus_.close(market_slice_subscription_);
        bus_.close(prices_subscription_);
        bus_.close(decision_subscription_);
        bus_.close(plan_subscription_);
        bus_.close(exchange_event_subscription_);
        bus_.close(exchange_snapshot_subscription_);
    }

    void run()
    {
        LG_INFO(
            "service=execution-state event=service_ready stream={} reconciliation=required strategies={} initial_cash={} poll_timeout_ms={}",
            options_.stream,
            options_.strategies.size(),
            options_.initial_cash,
            options_.poll_timeout_ms
        );

        while (running.load()) {
            // Exchange truth/events are deliberately processed before new plans. This,
            // combined with state_revision validation, minimizes stale-plan windows.
            bus_.poll(exchange_snapshot_subscription_, 16, options_.poll_timeout_ms);
            bus_.poll(exchange_event_subscription_, 64, options_.poll_timeout_ms);
            bus_.poll(plan_subscription_, 32, options_.poll_timeout_ms);
            bus_.poll(market_slice_subscription_, 32, options_.poll_timeout_ms);
            bus_.poll(decision_subscription_, 32, options_.poll_timeout_ms);
            bus_.poll(prices_subscription_, 32, options_.poll_timeout_ms);
        }

        LG_INFO("service=execution-state event=shutdown_requested");
        bus_.flush();
        LG_INFO("service=execution-state event=shutdown_complete");
    }
};

} // namespace


int main(int argc, char** argv)
{
    ServiceLogging::setup("execution-state");

    try {
        std::signal(SIGINT, stopHandler);
        std::signal(SIGTERM, stopHandler);
        ExecutionStateServiceRuntime runtime(parseOptions(argc, argv));
        runtime.run();
        return 0;
    }
    catch (const std::exception& error) {
        LG_ALERT("service=execution-state event=fatal error={}", error.what());
        return 1;
    }
}
