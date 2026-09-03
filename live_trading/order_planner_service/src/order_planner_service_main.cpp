#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

#include "contract_json_codec.h"
#include "nats_jetstream_message_bus.h"
#include "execution_planning_state.h"
#include "order_manager.h"
#include "order_planner_engine.h"
#include "order_planning.h"
#include "service_logging.h"
#include "strategy_position_snapshot.h"
#include "transport_subjects.h"


namespace {

std::atomic<bool> running{true};

void stopHandler(int)
{
    running.store(false);
}

struct Options {
    std::string nats_url = "nats://127.0.0.1:4222";
    std::string stream = "ALGOTRADING_RUNTIME";
    int poll_timeout_ms = 250;
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
        else if (arg == "--poll-timeout-ms")
            options.poll_timeout_ms = std::stoi(requireValue("--poll-timeout-ms"));
        else if (arg == "--help") {
            std::cout
                << "Usage: algotrading_order_planner_service [options]\n"
                << "  --nats-url URL\n"
                << "  --stream NAME\n"
                << "  --poll-timeout-ms VALUE\n";
            std::exit(0);
        }
        else
            throw std::invalid_argument("Unknown option: " + arg);
    }

    if (options.poll_timeout_ms <= 0)
        throw std::invalid_argument("--poll-timeout-ms must be positive");
    return options;
}


class OrderPlannerServiceRuntime {
private:
    Options options_;
    NatsJetStreamMessageBus bus_;
    OrderPlannerEngine planner_;
    DurableMessageBus::SubscriptionID request_subscription_ = 0;

    DurableConsumerOptions consumer() const
    {
        DurableConsumerOptions result;
        result.stream = options_.stream;
        result.durable_name = "order-planner-service-requests";
        result.subject = TransportSubjects::ORDER_PLANNING_REQUEST;
        result.ack_wait_ms = 30000;
        result.max_deliver = 20;
        result.max_ack_pending = 128;
        return result;
    }

    static StrategyPositionSnapshot strategyPositions(const OrderPlanningRequest& request)
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

    static void validate(const OrderPlanningRequest& request)
    {
        if (request.decision_timestamp == 0 || request.execution_timestamp == 0)
            throw std::invalid_argument("Planning request timestamps must be non-zero");
        if (request.decision_timestamp != request.decisions.decision_timestamp)
            throw std::invalid_argument("Planning request/decision timestamps do not match");
        if (request.decision_timestamp != request.prices.decision_timestamp)
            throw std::invalid_argument("Planning request/price decision timestamps do not match");
        if (request.execution_timestamp != request.prices.timestamp)
            throw std::invalid_argument("Planning request execution/price timestamps do not match");
        if (request.state.state_revision == 0)
            throw std::invalid_argument("Planning request state revision must be non-zero");
        if (executionPlanningStateRevision(request.state) != request.state.state_revision)
            throw std::invalid_argument("Planning request state revision does not match payload");
        if (request.state.strategy_ids.empty())
            throw std::invalid_argument("Planning request requires configured strategy ids");
        if (request.state.next_order_id == 0)
            throw std::invalid_argument("Planning request next order id must be non-zero");
        if (request.prices.prices.empty())
            throw std::invalid_argument("Planning request requires execution prices");

        std::unordered_set<StrategyID> ids;
        for (const StrategyID strategyId : request.state.strategy_ids) {
            if (!ids.insert(strategyId).second)
                throw std::invalid_argument("Planning request contains duplicate strategy id");
        }
    }

    DurableMessageDisposition onRequest(const BusMessage& message)
    {
        try {
            const OrderPlanningRequest request =
                ContractJsonCodec::decodeOrderPlanningRequest(message.payload);
            validate(request);

            LG_INFO(
                "service=order-planner event=planning_request_received decision_timestamp={} execution_timestamp={} state_revision={} strategies={} tracked_orders={} prices={} next_order_id={} message_id={}",
                request.decision_timestamp,
                request.execution_timestamp,
                request.state.state_revision,
                request.state.strategy_ids.size(),
                request.state.orders.size(),
                request.prices.prices.size(),
                request.state.next_order_id,
                request.metadata.message_id
            );

            ExecutionReferencePrices prices;
            for (const auto& [coin, price] : request.prices.prices)
                prices.set(coin, price);

            OrderManager orderManager;
            orderManager.restore(request.state.orders, {});

            const OrderPlannerResult planning = planner_.createPlan(
                request.state.strategy_ids,
                strategyPositions(request),
                orderManager,
                request.execution_timestamp,
                prices,
                request.decisions,
                request.state.next_order_id
            );

            OrderPlanBatch output;
            output.metadata.schema_version = 1;
            output.metadata.message_id = "order-plan:" + request.metadata.message_id;
            output.metadata.correlation_id = request.metadata.message_id;
            output.metadata.produced_at = request.execution_timestamp;
            output.decision_timestamp = request.decision_timestamp;
            output.execution_timestamp = request.execution_timestamp;
            output.state_revision = request.state.state_revision;
            output.decisions = request.decisions;
            output.prices = request.prices;
            output.next_order_id = planning.next_order_id;
            output.cancel_order_ids = planning.execution_plan.order_ids_to_cancel;
            output.submit_orders = planning.execution_plan.orders_to_submit;
            output.global_target_exposure = planning.global_target.values();

            bus_.publish(
                TransportSubjects::ORDER_PLAN,
                ContractJsonCodec::encode(output),
                output.metadata.message_id
            );

            LG_INFO(
                "service=order-planner event=order_plan_published decision_timestamp={} execution_timestamp={} state_revision={} cancels={} submits={} target_assets={} next_order_id={} message_id={} correlation_id={}",
                output.decision_timestamp,
                output.execution_timestamp,
                output.state_revision,
                output.cancel_order_ids.size(),
                output.submit_orders.size(),
                output.global_target_exposure.size(),
                output.next_order_id,
                output.metadata.message_id,
                output.metadata.correlation_id
            );
            for (const ExecutionOrder& order : output.submit_orders) {
                LG_DEBUG(
                    "service=order-planner event=planned_submit order_id={} strategy_id={} coin={} side={} quantity={} created_at={} active_from={}",
                    order.order_id,
                    order.strategy_id,
                    order.coin,
                    order.side == OrderSide::Buy ? "buy" : "sell",
                    order.quantity,
                    order.created_at,
                    order.active_from
                );
            }
            for (const OrderID orderId : output.cancel_order_ids)
                LG_DEBUG("service=order-planner event=planned_cancel order_id={}", orderId);

            return DurableMessageDisposition::Ack;
        }
        catch (const std::invalid_argument& error) {
            LG_WARN("service=order-planner event=planning_request_invalid disposition=terminate error={}", error.what());
            return DurableMessageDisposition::Terminate;
        }
        catch (const std::exception& error) {
            LG_ERROR("service=order-planner event=planning_request_failed disposition=retry error={}", error.what());
            return DurableMessageDisposition::Retry;
        }
    }

public:
    explicit OrderPlannerServiceRuntime(Options options)
        : options_(std::move(options)),
          bus_(options_.nats_url)
    {
        bus_.ensureStream(options_.stream, TransportSubjects::runtimeSubjects());
        request_subscription_ = bus_.subscribe(
            consumer(),
            [this](const BusMessage& message) { return onRequest(message); }
        );
    }

    ~OrderPlannerServiceRuntime()
    {
        bus_.close(request_subscription_);
    }

    void run()
    {
        LG_INFO(
            "service=order-planner event=service_ready stream={} poll_timeout_ms={}",
            options_.stream,
            options_.poll_timeout_ms
        );
        while (running.load())
            bus_.poll(request_subscription_, 32, options_.poll_timeout_ms);
        LG_INFO("service=order-planner event=shutdown_requested");
        bus_.flush();
        LG_INFO("service=order-planner event=shutdown_complete");
    }
};

} // namespace


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
        LG_ALERT("service=order-planner event=fatal error={}", error.what());
        return 1;
    }
}
