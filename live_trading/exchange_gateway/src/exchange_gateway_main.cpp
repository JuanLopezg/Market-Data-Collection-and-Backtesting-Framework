// Bridges northbound trading commands to an exchange backend and republishes exchange events.
//
// 1. Read runtime/control/backend stream configuration.
// 2. Subscribe to submit, cancel, snapshot, or dry-run planning messages.
// 3. Forward commands to the backend adapter and publish normalized order/fill/snapshot events.

#include <atomic>
#include <csignal>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "message_json.h"
#include "exchange_gateway_adapter.h"
#include "exchange_snapshot_request.h"
#include "adapters/backend_gateway.h"
#include "jetstream_bus.h"
#include "service_logging.h"
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

bool envFlag(const char* name)
{
    const char* value = std::getenv(name);
    if (value == nullptr)
        return false;
    const std::string text(value);
    return !text.empty() && text != "0" && text != "false" && text != "FALSE";
}

Timestamp currentBusinessUtcDate(const TimeHandler& timeHandler)
{
    return static_cast<Timestamp>(
        toYYYYMMDD(getCurrentUtcDate(timeHandler.getTime()))
    );
}

Timestamp newestCompletedUtcDate(const TimeHandler& timeHandler)
{
    const auto todayUtc = getCurrentUtcDate(timeHandler.getTime());
    return static_cast<Timestamp>(toYYYYMMDD(getPreviousDayDate(todayUtc)));
}


enum class GatewayMode {
    Backend,
    HyperliquidDryRun
};

GatewayMode parseGatewayMode(const std::string& value)
{
    if (value == "backend")
        return GatewayMode::Backend;
    if (value == "hyperliquid-dry-run")
        return GatewayMode::HyperliquidDryRun;

    throw std::invalid_argument(
        "--mode must be backend or hyperliquid-dry-run"
    );
}

const char* gatewayModeName(GatewayMode mode)
{
    switch (mode) {
    case GatewayMode::Backend:
        return "backend";
    case GatewayMode::HyperliquidDryRun:
        return "hyperliquid-dry-run";
    }
    return "unknown";
}


// Command-line configuration.

struct Options {
    std::string nats_url = "nats://127.0.0.1:4222";
    std::string runtime_stream = "ALGOTRADING_RUNTIME";
    std::string control_stream = "ALGOTRADING_EXCHANGE_CONTROL";
    std::string backend_stream = "ALGOTRADING_EXCHANGE_BACKEND";
    GatewayMode mode = GatewayMode::Backend;
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
            options.runtime_stream = requireValue("--stream");
        else if (arg == "--control-stream")
            options.control_stream = requireValue("--control-stream");
        else if (arg == "--backend-stream")
            options.backend_stream = requireValue("--backend-stream");
        else if (arg == "--mode")
            options.mode = parseGatewayMode(requireValue("--mode"));
        else if (arg == "--poll-timeout-ms")
            options.poll_timeout_ms = std::stoi(requireValue("--poll-timeout-ms"));
        else if (arg == "--help" || arg == "-h") {
            std::cout
                << "Exchange gateway options:\n"
                << "  --nats-url URL\n"
                << "  --stream NAME\n"
                << "  --control-stream NAME\n"
                << "  --backend-stream NAME\n"
                << "  --mode backend|hyperliquid-dry-run\n"
                << "  --poll-timeout-ms N\n";
            std::exit(0);
        }
        else
            throw std::invalid_argument("Unknown option: " + arg);
    }

    if (options.runtime_stream.empty() || options.control_stream.empty() ||
        options.backend_stream.empty())
        throw std::invalid_argument("Gateway stream names cannot be empty");
    if (options.poll_timeout_ms <= 0)
        throw std::invalid_argument("--poll-timeout-ms must be positive");

    return options;
}


// Service runtime and message-processing loop.

class ExchangeGatewayRuntime {
private:
    const Options options_;
    TimeHandler time_handler_;
    JetStreamBus bus_;
    std::unique_ptr<BackendGateway> adapter_;

    MessageBus::SubscriptionID submit_subscription_ = 0;
    MessageBus::SubscriptionID cancel_subscription_ = 0;
    MessageBus::SubscriptionID snapshot_request_subscription_ = 0;
    MessageBus::SubscriptionID notional_plan_subscription_ = 0;
    bool chaos_duplicate_fill_once_ = envFlag("ALGOTRADING_CHAOS_DUPLICATE_FILL_ONCE");
    bool chaos_duplicate_fill_done_ = false;

    DurableConsumerOptions consumer(
        const std::string& stream,
        const std::string& durable,
        const std::string& subject
    ) const
    {
        DurableConsumerOptions result;
        result.stream = stream;
        result.durable_name = durable;
        result.subject = subject;
        result.ack_wait_ms = 30000;
        result.max_deliver = 20;
        result.max_ack_pending = 256;
        return result;
    }

    static void validateMetadata(const ContractMetadata& metadata)
    {
        if (metadata.schema_version != 1 || metadata.message_id.empty())
            throw std::invalid_argument("Invalid exchange-gateway contract metadata");
    }

    void validateBackendEventTime(Timestamp eventTime, Timestamp producedAt, const char* eventName) const
    {
        if (eventTime == 0 || producedAt == 0)
            throw std::invalid_argument(std::string(eventName) + " timestamp must be non-zero");

        const Timestamp businessToday = currentBusinessUtcDate(time_handler_);
        if (eventTime > businessToday || producedAt > businessToday) {
            throw std::invalid_argument(
                std::string(eventName) + " is ahead of ExchangeGateway business time"
            );
        }
    }

    void publish(const OrderUpdateEvent& value)
    {
        validateMetadata(value.metadata);
        validateBackendEventTime(
            value.update.timestamp,
            value.metadata.produced_at,
            "OrderUpdateEvent"
        );
        bus_.publish(
            MessageSubjects::ORDER_UPDATE,
            MessageJson::encode(value),
            value.metadata.message_id
        );
        LG_INFO("Forwarded update: order {} is now {} ({})",
                value.update.order_id,
                executionOrderStatusName(value.update.status),
                FormatDayKey(value.update.timestamp));
        LG_DEBUG("Order update message id {}", value.metadata.message_id);
    }

    void publish(const FillEvent& value)
    {
        validateMetadata(value.metadata);
        validateBackendEventTime(
            value.fill.timestamp,
            value.metadata.produced_at,
            "FillEvent"
        );
        bus_.publish(
            MessageSubjects::FILL,
            MessageJson::encode(value),
            value.metadata.message_id
        );
        LG_INFO("Forwarded fill for order {}: {} {} {} at {}",
                value.fill.order_id,
                value.fill.side == OrderSide::Buy ? "BUY" : "SELL",
                FormatQuantity(value.fill.quantity),
                value.fill.coin,
                FormatQuantity(value.fill.price));
        LG_DEBUG("Fill id {}, message id {}", value.fill.fill_id, value.metadata.message_id);

        if (chaos_duplicate_fill_once_ && !chaos_duplicate_fill_done_) {
            FillEvent duplicate = value;
            duplicate.metadata.message_id += ":chaos-duplicate-transport";
            bus_.publish(
                MessageSubjects::FILL,
                MessageJson::encode(duplicate),
                duplicate.metadata.message_id
            );
            chaos_duplicate_fill_done_ = true;
            LG_WARN("Fault test: sent fill {} for order {} a second time", duplicate.fill.fill_id, duplicate.fill.order_id);
            LG_DEBUG("Duplicate fill message id {}", duplicate.metadata.message_id);
        }
    }

    void publish(const ExchangeSnapshotEvent& value)
    {
        validateMetadata(value.metadata);
        validateBackendEventTime(
            value.snapshot.timestamp,
            value.metadata.produced_at,
            "ExchangeSnapshotEvent"
        );
        bus_.publish(
            MessageSubjects::EXCHANGE_SNAPSHOT,
            MessageJson::encode(value),
            value.metadata.message_id
        );
        LG_INFO("Forwarded exchange snapshot for {}: cash {}, {} positions, {} open orders",
                FormatDayKey(value.snapshot.timestamp),
                FormatUsd(value.snapshot.cash),
                value.snapshot.positions.size(),
                value.snapshot.open_orders.size());
        LG_DEBUG("Exchange snapshot message id {}", value.metadata.message_id);
    }

    DurableMessageDisposition onSubmit(const BusMessage& message)
    {
        try {
            const SubmitOrderCommand command =
                MessageJson::decodeSubmitOrderCommand(message.payload);
            validateMetadata(command.metadata);
            if (command.order.order_id == 0)
                return DurableMessageDisposition::Terminate;

            // The adapter call returns only after its downstream command is durable.
            // We can therefore ACK this northbound command without losing it on crash.
            LG_INFO("Forwarding order {}: {} {} {} for {}, active from {}",
                    command.order.order_id,
                    command.order.side == OrderSide::Buy ? "BUY" : "SELL",
                    FormatQuantity(command.order.quantity),
                    command.order.coin,
                    command.order.strategy_id,
                    FormatDayKey(command.order.active_from));
            LG_DEBUG("Order {} submit message id {}", command.order.order_id, command.metadata.message_id);
            adapter_->submitOrder(command);
            LG_DEBUG("Order {} forwarded", command.order.order_id);
            return DurableMessageDisposition::Ack;
        }
        catch (const std::invalid_argument& error) {
            LG_WARN("Rejected invalid order: {}", error.what());
            return DurableMessageDisposition::Terminate;
        }
        catch (const std::exception& error) {
            LG_ERROR("Forwarding the order failed; will retry: {}", error.what());
            return DurableMessageDisposition::Retry;
        }
    }

    DurableMessageDisposition onCancel(const BusMessage& message)
    {
        try {
            const CancelOrderCommand command =
                MessageJson::decodeCancelOrderCommand(message.payload);
            validateMetadata(command.metadata);
            if (command.order_id == 0)
                return DurableMessageDisposition::Terminate;

            LG_INFO("Forwarding cancel for order {} ({})", command.order_id, FormatDayKey(command.requested_at));
            LG_DEBUG("Cancel message id {}", command.metadata.message_id);
            adapter_->cancelOrder(command);
            LG_DEBUG("Cancel for order {} forwarded", command.order_id);
            return DurableMessageDisposition::Ack;
        }
        catch (const std::invalid_argument& error) {
            LG_WARN("Rejected invalid cancel: {}", error.what());
            return DurableMessageDisposition::Terminate;
        }
        catch (const std::exception& error) {
            LG_ERROR("Forwarding the cancel failed; will retry: {}", error.what());
            return DurableMessageDisposition::Retry;
        }
    }

    DurableMessageDisposition onSnapshotRequest(const BusMessage& message)
    {
        try {
            const ExchangeSnapshotRequest request =
                MessageJson::decodeExchangeSnapshotRequest(message.payload);
            validateMetadata(request.metadata);
            LG_INFO("Forwarding exchange snapshot request");
            LG_DEBUG("Snapshot request message id {}, correlation id {}", request.metadata.message_id, request.metadata.correlation_id);
            adapter_->requestSnapshot(request);
            return DurableMessageDisposition::Ack;
        }
        catch (const std::invalid_argument& error) {
            LG_WARN("Rejected invalid snapshot request: {}", error.what());
            return DurableMessageDisposition::Terminate;
        }
        catch (const std::exception& error) {
            LG_ERROR("Forwarding the snapshot request failed; will retry: {}", error.what());
            return DurableMessageDisposition::Retry;
        }
    }

    DurableMessageDisposition onNotionalPlanDryRun(const BusMessage& message)
    {
        try {
            const NotionalOrderPlanBatch plan =
                MessageJson::decodeNotionalOrderPlanBatch(message.payload);

            validateMetadata(plan.metadata);

            const Timestamp newestCompleted = newestCompletedUtcDate(time_handler_);
            if (plan.decision_timestamp == 0 || plan.decision_timestamp > newestCompleted) {
                throw std::invalid_argument(
                    "NotionalOrderPlan decision_timestamp is ahead of the newest completed business day"
                );
            }

            LG_INFO("Hyperliquid dry run: received plan for {} with {} orders (newest completed day {}); preparing raw orders only",
                    FormatDayKey(plan.decision_timestamp), plan.submit_orders.size(), FormatDayKey(newestCompleted));
            LG_DEBUG("Dry-run plan state revision {}, message id {}", plan.state_revision, plan.metadata.message_id);

            for (const PlannedNotionalOrder& order : plan.submit_orders) {
                if (!std::isfinite(order.reference_close) ||
                    order.reference_close <= 0.0 ||
                    !std::isfinite(order.notional_usd) ||
                    order.notional_usd <= 0.0)
                    throw std::invalid_argument(
                        "Invalid notional/reference_close for Hyperliquid dry-run"
                    );

                const double rawQuantity =
                    order.notional_usd / order.reference_close;

                if (!std::isfinite(rawQuantity) || rawQuantity <= 0.0)
                    throw std::invalid_argument(
                        "Invalid raw Hyperliquid dry-run quantity"
                    );

                LG_INFO("Hyperliquid dry run: {} {} {} ({} at close {}) - venue rules not applied, not submitted",
                        order.side == OrderSide::Buy ? "BUY" : "SELL",
                        FormatQuantity(rawQuantity),
                        order.coin,
                        FormatUsd(order.notional_usd),
                        FormatQuantity(order.reference_close));
                LG_DEBUG("Dry-run order ids: economic {}, order {}; decision {}, state revision {}",
                         order.economic_order_id, order.order_id, FormatDayKey(order.decision_timestamp), order.state_revision);
            }

            return DurableMessageDisposition::Ack;
        }
        catch (const std::invalid_argument& error) {
            LG_WARN("Hyperliquid dry run: rejected invalid plan: {}", error.what());
            return DurableMessageDisposition::Terminate;
        }
        catch (const std::exception& error) {
            LG_ERROR("Hyperliquid dry run: plan handling failed; will retry: {}", error.what());
            return DurableMessageDisposition::Retry;
        }
    }

public:
    explicit ExchangeGatewayRuntime(Options options)
        : options_(std::move(options)),
          time_handler_(TimeHandlerFactory::createFromEnvironment()),
          bus_(options_.nats_url)
    {
        // Snapshot requests intentionally use a separate control stream.
        // This keeps control traffic isolated from normal runtime event subjects.
        // Gateway is bound only to normal trading subjects. Business/event-time
        // validation is local through TimeHandler; no shared-clock control plane is used.
        bus_.ensureStream(
            options_.runtime_stream,
            MessageSubjects::tradingRuntimeSubjects()
        );
        if (options_.mode == GatewayMode::Backend) {
            bus_.ensureStream(
                options_.control_stream,
                MessageSubjects::exchangeGatewayControlSubjects()
            );

            adapter_ = std::make_unique<BackendGateway>(
                options_.nats_url, options_.backend_stream
            );
            adapter_->setHandlers({
                [this](const OrderUpdateEvent& value) { publish(value); },
                [this](const FillEvent& value) { publish(value); },
                [this](const ExchangeSnapshotEvent& value) { publish(value); }
            });

            submit_subscription_ = bus_.subscribe(
                consumer(
                    options_.runtime_stream,
                    "exchange-gateway-submit",
                    MessageSubjects::SUBMIT_ORDER
                ),
                [this](const BusMessage& message) { return onSubmit(message); }
            );

            cancel_subscription_ = bus_.subscribe(
                consumer(
                    options_.runtime_stream,
                    "exchange-gateway-cancel",
                    MessageSubjects::CANCEL_ORDER
                ),
                [this](const BusMessage& message) { return onCancel(message); }
            );

            snapshot_request_subscription_ = bus_.subscribe(
                consumer(
                    options_.control_stream,
                    "exchange-gateway-snapshot-request",
                    MessageSubjects::EXCHANGE_SNAPSHOT_REQUEST
                ),
                [this](const BusMessage& message) {
                    return onSnapshotRequest(message);
                }
            );
        }
        else {
            // Dry-run mode can inspect and validate notional plans but cannot submit orders.
            // It deliberately has no SubmitOrder/CancelOrder/backend binding.
            notional_plan_subscription_ = bus_.subscribe(
                consumer(
                    options_.runtime_stream,
                    "exchange-gateway-hyperliquid-dry-run-plan",
                    MessageSubjects::NOTIONAL_ORDER_PLAN
                ),
                [this](const BusMessage& message) {
                    return onNotionalPlanDryRun(message);
                }
            );
        }
    }

    ~ExchangeGatewayRuntime()
    {
        if (notional_plan_subscription_ != 0)
            bus_.close(notional_plan_subscription_);
        if (snapshot_request_subscription_ != 0)
            bus_.close(snapshot_request_subscription_);
        if (cancel_subscription_ != 0)
            bus_.close(cancel_subscription_);
        if (submit_subscription_ != 0)
            bus_.close(submit_subscription_);
    }

    void run()
    {
        LG_INFO("Exchange gateway ready (mode {}, today {}, real submission disabled)",
                gatewayModeName(options_.mode), FormatDayKey(currentBusinessUtcDate(time_handler_)));
        LG_DEBUG("Runtime stream {}, control stream {}, backend stream {}, poll timeout {} ms",
                 options_.runtime_stream, options_.control_stream, options_.backend_stream, options_.poll_timeout_ms);
        std::cout.flush();

        while (running.load()) {
            if (options_.mode == GatewayMode::Backend) {
                // Existing replay/test path remains unchanged.
                adapter_->poll(options_.poll_timeout_ms);
                bus_.poll(
                    snapshot_request_subscription_,
                    8,
                    options_.poll_timeout_ms
                );
                bus_.poll(
                    cancel_subscription_,
                    32,
                    options_.poll_timeout_ms
                );
                bus_.poll(
                    submit_subscription_,
                    32,
                    options_.poll_timeout_ms
                );
            }
            else {
                // Dry-run boundary: there is no backend binding and no real submission path.
                bus_.poll(
                    notional_plan_subscription_,
                    32,
                    options_.poll_timeout_ms
                );
            }
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
    ServiceLogging::setup("exchange-gateway");

    try {
        std::signal(SIGINT, stopHandler);
        std::signal(SIGTERM, stopHandler);
        ExchangeGatewayRuntime runtime(parseOptions(argc, argv));
        runtime.run();
        return 0;
    }
    catch (const std::exception& error) {
        LG_ALERT("Exchange gateway stopped by a fatal error: {}", error.what());
        return 1;
    }
}
