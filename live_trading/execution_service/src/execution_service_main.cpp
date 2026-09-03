#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdlib>
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
#include "execution_engine.h"
#include "execution_price_snapshot.h"
#include "message_bus_exchange.h"
#include "nats_jetstream_message_bus.h"
#include "postgres_state_store.h"
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
        else if (arg == "--initial-cash")
            options.initial_cash = std::stod(requireValue("--initial-cash"));
        else if (arg == "--poll-timeout-ms")
            options.poll_timeout_ms = std::stoi(requireValue("--poll-timeout-ms"));
        else if (arg == "--strategy")
            options.strategies.push_back(parseStrategy(requireValue("--strategy")));
        else if (arg == "--help") {
            std::cout
                << "Usage: algotrading_execution_service [options]\n"
                << "  --nats-url URL\n"
                << "  --postgres CONNECTION_STRING\n"
                << "  --stream NAME\n"
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


class ExecutionServiceRuntime {
private:
    const Options options_;
    std::unordered_map<StrategyID, std::string> strategy_names_;

    NatsJetStreamMessageBus bus_;
    PostgresStateStore store_;
    Account account_;
    TradeRecorder trade_recorder_;
    MessageBusExchange exchange_;
    ExecutionEngine engine_;

    std::optional<DecisionBatch> pending_decision_;
    Timestamp last_decision_timestamp_ = 0;

    DurableMessageBus::SubscriptionID decision_subscription_ = 0;
    DurableMessageBus::SubscriptionID prices_subscription_ = 0;
    DurableMessageBus::SubscriptionID order_update_subscription_ = 0;
    DurableMessageBus::SubscriptionID fill_subscription_ = 0;

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
    }

    void restore(const TradingStateSnapshot& stored)
    {
        if (stored.schema_version != TradingStateSnapshot::CURRENT_SCHEMA_VERSION)
            throw std::invalid_argument("Unsupported execution-service state schema version");

        StrategyPositionSnapshot positions;
        for (const StrategyStateSnapshot& strategy : stored.strategies) {
            if (strategy_names_.find(strategy.strategy_id) == strategy_names_.end())
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

        if (!stored.pending_plans.empty()) {
            DecisionBatch restoredBatch;
            restoredBatch.decision_timestamp = stored.pending_plans.front().plan.timestamp();

            for (const PendingPlanSnapshot& pending : stored.pending_plans) {
                if (pending.plan.timestamp() != restoredBatch.decision_timestamp)
                    throw std::invalid_argument(
                        "Execution service state contains multiple pending decision timestamps"
                    );

                StrategyDecisionIntent intent;
                intent.strategy_id = pending.strategy_id;
                intent.decision_timestamp = pending.plan.timestamp();
                intent.reference_capital = pending.plan.referenceCapital();
                intent.decisions = pending.plan.values();
                restoredBatch.strategies.push_back(std::move(intent));
            }

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
            if (strategy_names_.find(intent.strategy_id) == strategy_names_.end())
                return false;
            if (!seen.insert(intent.strategy_id).second)
                return false;
            if (intent.decision_timestamp != batch.decision_timestamp)
                return false;
        }
        return true;
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
            return DurableMessageDisposition::Ack;
        }
        catch (const std::exception& error) {
            std::cerr << "Decision delivery failed: " << error.what() << '\n';
            return DurableMessageDisposition::Retry;
        }
    }

    DurableMessageDisposition onPrices(const BusMessage& message)
    {
        try {
            const ExecutionPriceSnapshot value =
                ContractJsonCodec::decodeExecutionPriceSnapshot(message.payload);

            if (value.timestamp == 0 || value.decision_timestamp == 0 || value.prices.empty())
                return DurableMessageDisposition::Terminate;

            if (value.timestamp < engine_.lastExecutionTimestamp())
                return DurableMessageDisposition::Ack;
            if (value.timestamp == engine_.lastExecutionTimestamp()) {
                // If a prior attempt committed state but failed before publishing the account
                // barrier/ACK, redelivery must recreate that durable side effect.
                publishAccountSnapshot(
                    value.timestamp,
                    "account-snapshot:execution:" + std::to_string(value.timestamp),
                    value.metadata.message_id
                );
                return DurableMessageDisposition::Ack;
            }

            ExecutionReferencePrices prices;
            for (const auto& [coin, price] : value.prices)
                prices.set(coin, price);

            if (!pending_decision_) {
                // A completed slice may legitimately produce no execution intent. Still
                // checkpoint execution time and publish the T account-state barrier used
                // by portfolio-risk at the close of the same timestamp.
                if (value.decision_timestamp > last_decision_timestamp_)
                    return DurableMessageDisposition::Retry;

                DecisionBatch emptyDecision;
                emptyDecision.decision_timestamp = value.decision_timestamp;
                engine_.executeDecisionBatch(
                    value.timestamp,
                    prices,
                    emptyDecision,
                    [this](const std::optional<Fill>& fill) { persist(fill); }
                );
                persist();
                publishAccountSnapshot(
                    value.timestamp,
                    "account-snapshot:execution:" + std::to_string(value.timestamp),
                    value.metadata.message_id
                );
                return DurableMessageDisposition::Ack;
            }

            if (pending_decision_->decision_timestamp != value.decision_timestamp)
                return DurableMessageDisposition::Retry;

            engine_.executeDecisionBatch(
                value.timestamp,
                prices,
                *pending_decision_,
                [this](const std::optional<Fill>& fill) { persist(fill); }
            );

            pending_decision_.reset();
            persist();
            publishAccountSnapshot(
                value.timestamp,
                "account-snapshot:execution:" + std::to_string(value.timestamp),
                value.metadata.message_id
            );
            return DurableMessageDisposition::Ack;
        }
        catch (const std::exception& error) {
            std::cerr << "Execution-price delivery failed: " << error.what() << '\n';
            return DurableMessageDisposition::Retry;
        }
    }

    DurableMessageDisposition onOrderUpdate(const BusMessage& message)
    {
        try {
            const OrderUpdateEvent value = ContractJsonCodec::decodeOrderUpdateEvent(message.payload);
            engine_.processExchangeEvent(
                ExchangeEvent{value.update},
                [this](const std::optional<Fill>& fill) { persist(fill); }
            );
            return DurableMessageDisposition::Ack;
        }
        catch (const std::exception& error) {
            std::cerr << "Order-update delivery failed: " << error.what() << '\n';
            return DurableMessageDisposition::Retry;
        }
    }

    DurableMessageDisposition onFill(const BusMessage& message)
    {
        try {
            const FillEvent value = ContractJsonCodec::decodeFillEvent(message.payload);
            engine_.processExchangeEvent(
                ExchangeEvent{value.fill},
                [this](const std::optional<Fill>& fill) { persist(fill); }
            );
            publishAccountSnapshot(
                value.fill.timestamp,
                "account-snapshot:fill:" + std::to_string(value.fill.fill_id),
                value.metadata.message_id
            );
            return DurableMessageDisposition::Ack;
        }
        catch (const std::exception& error) {
            std::cerr << "Fill delivery failed: " << error.what() << '\n';
            return DurableMessageDisposition::Retry;
        }
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
    explicit ExecutionServiceRuntime(Options options)
        : options_(std::move(options)),
          bus_(options_.nats_url),
          store_(options_.postgres_connection),
          account_(options_.initial_cash),
          exchange_(bus_),
          engine_(options_.strategies, account_, trade_recorder_, exchange_)
    {
        for (const auto& strategy : options_.strategies)
            strategy_names_.emplace(strategy.strategy_id, strategy.name);

        bus_.ensureStream(options_.stream, TransportSubjects::runtimeSubjects());

        if (const auto stored = store_.load())
            restore(*stored);
        else
            persist();

        decision_subscription_ = bus_.subscribe(
            consumer("execution-service-decisions", TransportSubjects::DECISION_BATCH),
            [this](const BusMessage& message) { return onDecision(message); }
        );
        prices_subscription_ = bus_.subscribe(
            consumer("execution-service-prices", TransportSubjects::EXECUTION_PRICES),
            [this](const BusMessage& message) { return onPrices(message); }
        );
        order_update_subscription_ = bus_.subscribe(
            consumer("execution-service-order-updates", TransportSubjects::ORDER_UPDATE),
            [this](const BusMessage& message) { return onOrderUpdate(message); }
        );
        fill_subscription_ = bus_.subscribe(
            consumer("execution-service-fills", TransportSubjects::FILL),
            [this](const BusMessage& message) { return onFill(message); }
        );
    }

    ~ExecutionServiceRuntime()
    {
        bus_.close(fill_subscription_);
        bus_.close(order_update_subscription_);
        bus_.close(prices_subscription_);
        bus_.close(decision_subscription_);
    }

    void run()
    {
        std::cout << "Execution service ready. stream=" << options_.stream << std::endl;

        while (running.load()) {
            // Decisions are polled before execution prices. If network ordering still
            // delivers a price first, Retry lets JetStream redeliver it after the decision.
            bus_.poll(decision_subscription_, 32, options_.poll_timeout_ms);
            bus_.poll(prices_subscription_, 32, options_.poll_timeout_ms);
            bus_.poll(order_update_subscription_, 32, options_.poll_timeout_ms);
            bus_.poll(fill_subscription_, 32, options_.poll_timeout_ms);
        }

        bus_.flush();
    }
};

} // namespace


int main(int argc, char** argv)
{
    try {
        std::signal(SIGINT, stopHandler);
        std::signal(SIGTERM, stopHandler);

        ExecutionServiceRuntime runtime(parseOptions(argc, argv));
        runtime.run();
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "Execution service fatal error: " << error.what() << '\n';
        return 1;
    }
}
