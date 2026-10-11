// Converts notional planning requests into deterministic executable notional order plans.
//
// 1. Recover the latest durable planning checkpoint.
// 2. Validate a planning request against current execution state.
// 3. Build, persist, and publish the notional order plan exactly once per business date.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include <libpq-fe.h>

#include "message_json.h"
#include "planning/planning_state.h"
#include "price_snapshot.h"
#include "live_execution_identity.h"
#include "jetstream_bus.h"
#include "planning/notional_order_planner.h"
#include "notional_order_planning.h"
#include "order_manager.h"
#include "service_logging.h"
#include "position_state.h"
#include "time_handler_factory.h"
#include "time_utils.h"
#include "message_subjects.h"


// Internal helpers and service implementation.

namespace {

std::atomic<bool> running{true};

void stopHandler(int)
{
    running.store(false);
}

Timestamp newestCompletedBusinessUtcDate(const TimeHandler& timeHandler)
{
    const auto todayUtc = getCurrentUtcDate(timeHandler.getTime());
    return static_cast<Timestamp>(toYYYYMMDD(getPreviousDayDate(todayUtc)));
}


std::optional<Timestamp> configuredReplayBootstrapCompletedUtcDate(const TimeHandlerConfig& config)
{
    const char* simulatedReference = std::getenv(TimeHandlerFactory::SIMULATED_REFERENCE_ENV);
    if (!simulatedReference || *simulatedReference == '\0')
        return std::nullopt;

    const auto referenceUtcDate = getCurrentUtcDate(config.simulated_reference_utc);
    return static_cast<Timestamp>(toYYYYMMDD(getPreviousDayDate(referenceUtcDate)));
}

void interruptibleSleepFor(std::chrono::steady_clock::duration duration)
{
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (running.load()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            return;
        const auto remaining = deadline - now;
        const auto chunk = std::min(
            std::chrono::duration_cast<std::chrono::milliseconds>(remaining),
            std::chrono::milliseconds(100));
        if (chunk.count() > 0)
            std::this_thread::sleep_for(chunk);
    }
}

// BUSINESS wait: the deadline is evaluated against TimeHandler business time.
// The short real sleep is only a TECHNICAL polling cadence so shutdown remains responsive.
void interruptibleBusinessWaitUntil(
    const TimeHandler& timeHandler,
    std::chrono::system_clock::time_point businessDeadline)
{
    constexpr auto technicalPollInterval = std::chrono::milliseconds(250);

    while (running.load()) {
        if (timeHandler.getTime() >= businessDeadline)
            return;
        std::this_thread::sleep_for(technicalPollInterval);
    }
}


// Command-line configuration.

struct Options {
    std::string nats_url = "nats://127.0.0.1:4222";
    std::string stream = "ALGOTRADING_RUNTIME";
    std::string postgres =
        "host=127.0.0.1 port=5432 dbname=algotrading user=algotrading password=algotrading";
};

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
        else if (arg == "--stream")
            options.stream = requireValue("--stream");
        else if (arg == "--postgres")
            options.postgres = requireValue("--postgres");
        else if (arg == "--help") {
            std::cout
                << "Usage: algotrading_order_planner_service [options]\n"
                << "  --nats-url URL\n"
                << "  --stream NAME\n"
                << "  --postgres CONNECTION_STRING\n"
                << "  Business time is provided by TimeHandler; technical polling remains real-time.\n";
            std::exit(0);
        }
        else
            throw std::invalid_argument("Unknown option: " + arg);
    }

    if (options.stream.empty())
        throw std::invalid_argument("--stream cannot be empty");
    if (options.postgres.empty())
        throw std::invalid_argument("--postgres is required for LIVE planner checkpoints");
    return options;
}


// Durable PostgreSQL persistence helpers.

class PgResult {
private:
    PGresult* result_ = nullptr;
public:
    explicit PgResult(PGresult* result) : result_(result) {}
    ~PgResult() { if (result_) PQclear(result_); }
    PgResult(const PgResult&) = delete;
    PgResult& operator=(const PgResult&) = delete;
    PgResult(PgResult&& other) noexcept : result_(std::exchange(other.result_, nullptr)) {}
    PgResult& operator=(PgResult&& other) noexcept
    {
        if (this != &other) {
            if (result_) PQclear(result_);
            result_ = std::exchange(other.result_, nullptr);
        }
        return *this;
    }
    PGresult* get() const { return result_; }
};

class OrderPlannerCheckpointStore {
private:
    PGconn* connection_ = nullptr;
    static constexpr const char* STATE_KEY = "order-planner-live-notional-v1";

    void requireConnection() const
    {
        if (connection_ == nullptr || PQstatus(connection_) != CONNECTION_OK)
            throw std::runtime_error("Order-planner PostgreSQL connection is not ready");
    }

    PgResult exec(const std::string& sql, ExecStatusType expected) const
    {
        requireConnection();
        PgResult result(PQexec(connection_, sql.c_str()));
        if (result.get() == nullptr || PQresultStatus(result.get()) != expected)
            throw std::runtime_error(
                "Order-planner PostgreSQL query failed: " +
                std::string(connection_ ? PQerrorMessage(connection_) : "unknown PostgreSQL error"));
        return result;
    }

    PgResult execParams(
        const std::string& sql,
        const std::vector<std::string>& parameters,
        ExecStatusType expected) const
    {
        requireConnection();
        std::vector<const char*> values;
        values.reserve(parameters.size());
        for (const std::string& parameter : parameters)
            values.push_back(parameter.c_str());

        PgResult result(PQexecParams(
            connection_, sql.c_str(), static_cast<int>(values.size()), nullptr,
            values.data(), nullptr, nullptr, 0));
        if (result.get() == nullptr || PQresultStatus(result.get()) != expected)
            throw std::runtime_error(
                "Order-planner PostgreSQL parameterized query failed: " +
                std::string(connection_ ? PQerrorMessage(connection_) : "unknown PostgreSQL error"));
        return result;
    }

    static Timestamp parseTimestamp(const char* value)
    {
        const unsigned long long parsed = std::stoull(value);
        if (parsed > static_cast<unsigned long long>(std::numeric_limits<Timestamp>::max()))
            throw std::runtime_error("Persisted order-planner timestamp is out of range");
        return static_cast<Timestamp>(parsed);
    }

    void ensureSchema()
    {
        exec(
            "CREATE TABLE IF NOT EXISTS order_planner_live_notional_checkpoint ("
            "state_key TEXT NOT NULL, timestamp BIGINT NOT NULL, "
            "request_payload TEXT NOT NULL, plan_payload TEXT NOT NULL, "
            "PRIMARY KEY(state_key,timestamp))",
            PGRES_COMMAND_OK);
    }

public:
    struct Row {
        Timestamp timestamp = 0;
        std::string request_payload;
        std::string plan_payload;
    };

    explicit OrderPlannerCheckpointStore(const std::string& connectionString)
    {
        connection_ = PQconnectdb(connectionString.c_str());
        if (connection_ == nullptr || PQstatus(connection_) != CONNECTION_OK) {
            const std::string error = connection_ ? PQerrorMessage(connection_) : "cannot allocate PGconn";
            if (connection_) { PQfinish(connection_); connection_ = nullptr; }
            throw std::runtime_error("Cannot connect order-planner checkpoint store to PostgreSQL: " + error);
        }
        ensureSchema();
    }

    ~OrderPlannerCheckpointStore() { if (connection_) PQfinish(connection_); }
    OrderPlannerCheckpointStore(const OrderPlannerCheckpointStore&) = delete;
    OrderPlannerCheckpointStore& operator=(const OrderPlannerCheckpointStore&) = delete;

    std::optional<Row> rowFor(Timestamp timestamp) const
    {
        const PgResult result = execParams(
            "SELECT request_payload,plan_payload FROM order_planner_live_notional_checkpoint "
            "WHERE state_key=$1 AND timestamp=$2",
            {STATE_KEY, std::to_string(timestamp)}, PGRES_TUPLES_OK);
        if (PQntuples(result.get()) == 0)
            return std::nullopt;
        if (PQntuples(result.get()) != 1)
            throw std::runtime_error("Order-planner checkpoint timestamp is not unique");
        Row row;
        row.timestamp = timestamp;
        row.request_payload = PQgetvalue(result.get(), 0, 0);
        row.plan_payload = PQgetvalue(result.get(), 0, 1);
        return row;
    }

    Timestamp latestTimestamp() const
    {
        const PgResult result = execParams(
            "SELECT COALESCE(MAX(timestamp),0) FROM order_planner_live_notional_checkpoint "
            "WHERE state_key=$1",
            {STATE_KEY}, PGRES_TUPLES_OK);
        if (PQntuples(result.get()) != 1)
            throw std::runtime_error("Order-planner checkpoint aggregate failed");
        return parseTimestamp(PQgetvalue(result.get(), 0, 0));
    }

    void save(Timestamp timestamp, const std::string& requestPayload, const std::string& planPayload)
    {
        execParams(
            "INSERT INTO order_planner_live_notional_checkpoint("
            "state_key,timestamp,request_payload,plan_payload) VALUES($1,$2,$3,$4) "
            "ON CONFLICT(state_key,timestamp) DO NOTHING",
            {STATE_KEY, std::to_string(timestamp), requestPayload, planPayload},
            PGRES_COMMAND_OK);

        const auto persisted = rowFor(timestamp);
        if (!persisted.has_value() || persisted->request_payload != requestPayload ||
            persisted->plan_payload != planPayload)
            throw std::logic_error("Conflicting persisted order-planner LIVE checkpoint");
    }
};


// Service runtime and message-processing loop.

class OrderPlannerServiceRuntime {
private:
    Options options_;
    const TimeHandlerConfig time_config_;
    const TimeHandler time_handler_;
    const std::optional<Timestamp> replay_bootstrap_completed_date_;
    JetStreamBus bus_;
    NotionalOrderPlanner planner_;
    OrderPlannerCheckpointStore checkpoint_store_;
    MessageBus::SubscriptionID request_subscription_ = 0;
    Timestamp latest_checkpoint_ = 0;

    DurableConsumerOptions consumer() const
    {
        DurableConsumerOptions result;
        result.stream = options_.stream;
        result.durable_name = "order-planner-live-notional-requests";
        result.subject = MessageSubjects::NOTIONAL_ORDER_PLANNING_REQUEST;
        result.ack_wait_ms = 30000;
        result.max_deliver = 20;
        result.max_ack_pending = 64;
        return result;
    }

    static StrategyPositionSnapshot strategyPositions(const NotionalOrderPlanningRequest& request)
    {
        StrategyPositionSnapshot result;
        for (const auto& [strategyId, positions] : request.state.strategy_positions) {
            VirtualPositionState state;
            for (const auto& [coin, quantity] : positions)
                state.set(coin, quantity);
            result.emplace(strategyId, std::move(state));
        }
        return result;
    }

    static std::unordered_set<Coin> requiredReferenceCloseSymbols(
        const NotionalOrderPlanningRequest& request)
    {
        std::unordered_set<Coin> required;

        for (const StrategyDecisionIntent& intent : request.decisions.strategies) {
            for (const auto& [coin, decision] : intent.decisions) {
                (void)decision;
                required.insert(coin);
            }
        }

        for (const auto& [strategyId, positions] : request.state.strategy_positions) {
            (void)strategyId;
            for (const auto& [coin, quantity] : positions) {
                (void)quantity;
                required.insert(coin);
            }
        }

        for (const TrackedOrder& tracked : request.state.orders) {
            if (tracked.isOpen())
                required.insert(tracked.request.coin);
        }

        return required;
    }

    static void validate(const NotionalOrderPlanningRequest& request)
    {
        if (request.metadata.schema_version != 1 || request.metadata.message_id.empty())
            throw std::invalid_argument("Notional planning request metadata is invalid");
        if (request.metadata.message_id != LiveExecutionIdentity::notionalPlanningRequest(
                request.decision_timestamp, request.state.state_revision))
            throw std::invalid_argument("Notional planning request deterministic identity mismatch");
        if (request.decision_timestamp == 0)
            throw std::invalid_argument("Notional planning request timestamp must be non-zero");
        if (request.decisions.decision_timestamp != request.decision_timestamp)
            throw std::invalid_argument("Notional planning request/decision timestamps do not match");
        if (request.reference_closes.date != request.decision_timestamp)
            throw std::invalid_argument("Notional planning request close date does not match decision date");
        if (request.state.state_revision == 0)
            throw std::invalid_argument("Notional planning request state revision must be non-zero");
        if (executionPlanningStateRevision(request.state) != request.state.state_revision)
            throw std::invalid_argument("Notional planning state revision does not match payload");
        if (request.state.strategy_ids.empty())
            throw std::invalid_argument("Notional planning request requires configured strategy ids");
        if (request.state.next_order_id == 0)
            throw std::invalid_argument("Notional planning request next order id must be non-zero");

        std::unordered_set<StrategyID> ids;
        for (const StrategyID strategyId : request.state.strategy_ids) {
            if (!ids.insert(strategyId).second)
                throw std::invalid_argument("Notional planning request contains duplicate strategy id");
        }

        for (const auto& [coin, close] : request.reference_closes.closes) {
            if (coin.empty() || !std::isfinite(close) || close <= 0.0)
                throw std::invalid_argument("Notional planning request contains invalid close(T)");
        }

        const std::unordered_set<Coin> required = requiredReferenceCloseSymbols(request);
        if (request.reference_closes.closes.size() != required.size())
            throw std::invalid_argument(
                "Notional planning request close(T) snapshot contains missing or unrelated symbols");

        for (const Coin& coin : required) {
            if (!request.reference_closes.closes.contains(coin))
                throw std::invalid_argument(
                    "Notional planning request is missing exact close(T) for " + coin);
        }
    }

    DurableMessageDisposition onRequest(const BusMessage& message)
    {
        try {
            const NotionalOrderPlanningRequest request =
                MessageJson::decodeNotionalOrderPlanningRequest(message.payload);
            validate(request);

            const Timestamp newestCompleted = newestCompletedBusinessUtcDate(time_handler_);
            if (request.decision_timestamp > newestCompleted) {
                LG_WARN("Rejected planning request for {}: that day is not complete yet (newest completed day {})",
                        FormatDayKey(request.decision_timestamp), FormatDayKey(newestCompleted));
                return DurableMessageDisposition::Terminate;
            }
            if (replay_bootstrap_completed_date_.has_value() &&
                request.decision_timestamp < *replay_bootstrap_completed_date_) {
                LG_INFO("Skipped planning request for {}: it is before the bootstrap day {}",
                        FormatDayKey(request.decision_timestamp), FormatDayKey(*replay_bootstrap_completed_date_));
                return DurableMessageDisposition::Ack;
            }
            if (!replay_bootstrap_completed_date_.has_value() &&
                request.decision_timestamp < newestCompleted) {
                LG_INFO("Skipped stale planning request for {} (newest completed day {})",
                        FormatDayKey(request.decision_timestamp), FormatDayKey(newestCompleted));
                return DurableMessageDisposition::Ack;
            }

            if (const auto persisted = checkpoint_store_.rowFor(request.decision_timestamp)) {
                if (persisted->request_payload != message.payload) {
                    LG_ALERT("Planning request for {} differs from the saved checkpoint; rejecting it",
                             FormatDayKey(request.decision_timestamp));
                    return DurableMessageDisposition::Terminate;
                }
                latest_checkpoint_ = std::max(latest_checkpoint_, request.decision_timestamp);
                LG_INFO("Planning request for {} was already processed; ignoring the duplicate",
                        FormatDayKey(request.decision_timestamp));
                return DurableMessageDisposition::Ack;
            }

            ExecutionReferencePrices closes;
            for (const auto& [coin, close] : request.reference_closes.closes)
                closes.set(coin, close);

            OrderManager orderManager;
            orderManager.restore(request.state.orders, {});

            const NotionalOrderPlannerResult planning = planner_.createPlan(
                request.state.strategy_ids,
                strategyPositions(request),
                orderManager,
                request.decision_timestamp,
                request.state.state_revision,
                closes,
                request.decisions,
                request.state.next_order_id
            );

            NotionalOrderPlanBatch output;
            output.metadata.schema_version = 1;
            output.metadata.message_id = LiveExecutionIdentity::notionalOrderPlan(
                request.decision_timestamp, request.state.state_revision);
            output.metadata.correlation_id = request.metadata.message_id;
            output.metadata.produced_at = request.decision_timestamp;
            output.decision_timestamp = request.decision_timestamp;
            output.state_revision = request.state.state_revision;
            output.decisions = request.decisions;
            output.reference_closes = request.reference_closes;
            output.next_order_id = planning.next_order_id;
            output.cancel_order_ids = planning.cancel_order_ids;
            output.submit_orders = planning.submit_orders;
            output.global_target_notional_usd = planning.global_target.values();

            const std::string encoded = MessageJson::encode(output);
            bus_.publish(
                MessageSubjects::NOTIONAL_ORDER_PLAN,
                encoded,
                output.metadata.message_id
            );
            bus_.flush();

            // Publish first, checkpoint second. A crash between the two republishes the
            // same deterministic MessageID after restart instead of creating new economics.
            checkpoint_store_.save(request.decision_timestamp, message.payload, encoded);
            latest_checkpoint_ = request.decision_timestamp;

            LG_INFO("Published order plan for {}: {} new orders, {} cancels, {} target assets",
                    FormatDayKey(output.decision_timestamp),
                    output.submit_orders.size(),
                    output.cancel_order_ids.size(),
                    output.global_target_notional_usd.size());
            LG_DEBUG("Order plan for {}: state revision {}, next order id {}, message id {}",
                     FormatDayKey(output.decision_timestamp),
                     output.state_revision,
                     output.next_order_id,
                     output.metadata.message_id);
            for (const PlannedNotionalOrder& order : output.submit_orders) {
                LG_INFO("Planned {} {} for {}: change {} (target {}, current {}, pending {}, close {})",
                        order.side == OrderSide::Buy ? "BUY" : "SELL",
                        order.coin,
                        order.strategy_id,
                        FormatUsd(order.delta_notional_usd),
                        FormatUsd(order.target_notional_usd),
                        FormatUsd(order.current_notional_usd),
                        FormatUsd(order.pending_notional_usd),
                        FormatQuantity(order.reference_close));
                LG_DEBUG("Planned {} order ids: economic {}, order {}; exact delta {} USD",
                         order.coin, order.economic_order_id, order.order_id, order.delta_notional_usd);
            }

            return DurableMessageDisposition::Ack;
        }
        catch (const std::invalid_argument& error) {
            LG_WARN("Rejected invalid planning request: {}", error.what());
            return DurableMessageDisposition::Terminate;
        }
        catch (const std::exception& error) {
            LG_ERROR("Planning request failed, will retry: {}", error.what());
            return DurableMessageDisposition::Retry;
        }
    }

public:
    explicit OrderPlannerServiceRuntime(Options options)
        : options_(std::move(options)),
          time_config_(TimeHandlerFactory::loadConfigFromEnvironment()),
          time_handler_(TimeHandlerFactory::create(time_config_)),
          replay_bootstrap_completed_date_(configuredReplayBootstrapCompletedUtcDate(time_config_)),
          bus_(options_.nats_url),
          checkpoint_store_(options_.postgres)
    {
        bus_.ensureStream(options_.stream, MessageSubjects::tradingRuntimeSubjects());
        latest_checkpoint_ = checkpoint_store_.latestTimestamp();
        request_subscription_ = bus_.subscribe(consumer(), [this](const BusMessage& message) {
            return onRequest(message);
        });
    }

    ~OrderPlannerServiceRuntime()
    {
        bus_.close(request_subscription_);
    }

    void run()
    {
        LG_INFO("Order planner ready (stream {}, last completed day {}, time speed {})",
                options_.stream, FormatDayKey(latest_checkpoint_), time_config_.speed);
        LG_DEBUG("Order planner time identity {}", time_config_.identity());
        std::cout.flush();

        bool waitingForNextUtcDayLogged = false;
        Timestamp loggedCompletedDate = 0;

        while (running.load()) {
            const Timestamp target = newestCompletedBusinessUtcDate(time_handler_);

            if (latest_checkpoint_ >= target) {
                if (!waitingForNextUtcDayLogged || loggedCompletedDate != target) {
                    LG_INFO("Day {} is planned; waiting for the next UTC day", FormatDayKey(latest_checkpoint_));
                    waitingForNextUtcDayLogged = true;
                    loggedCompletedDate = target;
                }
                interruptibleBusinessWaitUntil(
                    time_handler_,
                    computeNextMidnightUTC(time_handler_.getTime()));
                continue;
            }

            waitingForNextUtcDayLogged = false;

            const auto cycleStart = std::chrono::steady_clock::now();
            bus_.poll(request_subscription_, 32, 1);

            const auto elapsed = std::chrono::steady_clock::now() - cycleStart;
            const auto interval = std::chrono::seconds(1);
            if (elapsed < interval)
                interruptibleSleepFor(interval - elapsed);
        }

        LG_INFO("Shutdown requested");
        bus_.flush();
        LG_INFO("Shutdown complete");
    }
};

} // namespace


// Process entrypoint.

int main(int argc, char** argv)
{
    ServiceLogging::setup("order-planner");

    try {
        std::signal(SIGINT, stopHandler);
        std::signal(SIGTERM, stopHandler);
        OrderPlannerServiceRuntime runtime(parseOptions(argc, argv));
        runtime.run();
        return 0;
    }
    catch (const std::exception& error) {
        LG_ALERT("Order planner stopped by a fatal error: {}", error.what());
        return 1;
    }
}
