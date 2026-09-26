#include <atomic>
#include <csignal>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "contract_json_codec.h"
#include "exchange_gateway_adapter.h"
#include "exchange_snapshot_request.h"
#include "nats_backend_exchange_gateway_adapter.h"
#include "nats_jetstream_message_bus.h"
#include "service_logging.h"
#include "time_handler_factory.h"
#include "time_utils.h"
#include "transport_subjects.h"


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


class ExchangeGatewayRuntime {
private:
    const Options options_;
    TimeHandler time_handler_;
    NatsJetStreamMessageBus bus_;
    std::unique_ptr<NatsBackendExchangeGatewayAdapter> adapter_;

    DurableMessageBus::SubscriptionID submit_subscription_ = 0;
    DurableMessageBus::SubscriptionID cancel_subscription_ = 0;
    DurableMessageBus::SubscriptionID snapshot_request_subscription_ = 0;
    DurableMessageBus::SubscriptionID notional_plan_subscription_ = 0;
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
            TransportSubjects::ORDER_UPDATE,
            ContractJsonCodec::encode(value),
            value.metadata.message_id
        );
        LG_INFO(
            "service=exchange-gateway event=order_update_published order_id={} timestamp={} status={} message_id={}",
            value.update.order_id,
            value.update.timestamp,
            static_cast<int>(value.update.status),
            value.metadata.message_id
        );
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
            TransportSubjects::FILL,
            ContractJsonCodec::encode(value),
            value.metadata.message_id
        );
        LG_INFO(
            "service=exchange-gateway event=fill_published fill_id={} order_id={} coin={} side={} quantity={} price={} message_id={}",
            value.fill.fill_id,
            value.fill.order_id,
            value.fill.coin,
            value.fill.side == OrderSide::Buy ? "buy" : "sell",
            value.fill.quantity,
            value.fill.price,
            value.metadata.message_id
        );

        if (chaos_duplicate_fill_once_ && !chaos_duplicate_fill_done_) {
            FillEvent duplicate = value;
            duplicate.metadata.message_id += ":chaos-duplicate-transport";
            bus_.publish(
                TransportSubjects::FILL,
                ContractJsonCodec::encode(duplicate),
                duplicate.metadata.message_id
            );
            chaos_duplicate_fill_done_ = true;
            LG_WARN(
                "service=exchange-gateway event=chaos_duplicate_fill_injected fill_id={} order_id={} duplicate_message_id={}",
                duplicate.fill.fill_id,
                duplicate.fill.order_id,
                duplicate.metadata.message_id
            );
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
            TransportSubjects::EXCHANGE_SNAPSHOT,
            ContractJsonCodec::encode(value),
            value.metadata.message_id
        );
        LG_INFO(
            "service=exchange-gateway event=exchange_snapshot_published timestamp={} cash={} positions={} open_orders={} message_id={}",
            value.snapshot.timestamp,
            value.snapshot.cash,
            value.snapshot.positions.size(),
            value.snapshot.open_orders.size(),
            value.metadata.message_id
        );
    }

    DurableMessageDisposition onSubmit(const BusMessage& message)
    {
        try {
            const SubmitOrderCommand command =
                ContractJsonCodec::decodeSubmitOrderCommand(message.payload);
            validateMetadata(command.metadata);
            if (command.order.order_id == 0)
                return DurableMessageDisposition::Terminate;

            // The adapter call returns only after its downstream command is durable.
            // We can therefore ACK this northbound command without losing it on crash.
            LG_INFO(
                "service=exchange-gateway event=submit_forwarding order_id={} strategy_id={} coin={} side={} quantity={} active_from={} message_id={}",
                command.order.order_id,
                command.order.strategy_id,
                command.order.coin,
                command.order.side == OrderSide::Buy ? "buy" : "sell",
                command.order.quantity,
                command.order.active_from,
                command.metadata.message_id
            );
            adapter_->submitOrder(command);
            LG_DEBUG("service=exchange-gateway event=submit_forwarded order_id={} disposition=ack", command.order.order_id);
            return DurableMessageDisposition::Ack;
        }
        catch (const std::invalid_argument& error) {
            LG_WARN("service=exchange-gateway event=submit_invalid disposition=terminate error={}", error.what());
            return DurableMessageDisposition::Terminate;
        }
        catch (const std::exception& error) {
            LG_ERROR("service=exchange-gateway event=submit_failed disposition=retry error={}", error.what());
            return DurableMessageDisposition::Retry;
        }
    }

    DurableMessageDisposition onCancel(const BusMessage& message)
    {
        try {
            const CancelOrderCommand command =
                ContractJsonCodec::decodeCancelOrderCommand(message.payload);
            validateMetadata(command.metadata);
            if (command.order_id == 0)
                return DurableMessageDisposition::Terminate;

            LG_INFO(
                "service=exchange-gateway event=cancel_forwarding order_id={} requested_at={} message_id={}",
                command.order_id,
                command.requested_at,
                command.metadata.message_id
            );
            adapter_->cancelOrder(command);
            LG_DEBUG("service=exchange-gateway event=cancel_forwarded order_id={} disposition=ack", command.order_id);
            return DurableMessageDisposition::Ack;
        }
        catch (const std::invalid_argument& error) {
            LG_WARN("service=exchange-gateway event=cancel_invalid disposition=terminate error={}", error.what());
            return DurableMessageDisposition::Terminate;
        }
        catch (const std::exception& error) {
            LG_ERROR("service=exchange-gateway event=cancel_failed disposition=retry error={}", error.what());
            return DurableMessageDisposition::Retry;
        }
    }

    DurableMessageDisposition onSnapshotRequest(const BusMessage& message)
    {
        try {
            const ExchangeSnapshotRequest request =
                ContractJsonCodec::decodeExchangeSnapshotRequest(message.payload);
            validateMetadata(request.metadata);
            LG_INFO(
                "service=exchange-gateway event=snapshot_request_forwarding message_id={} correlation_id={}",
                request.metadata.message_id,
                request.metadata.correlation_id
            );
            adapter_->requestSnapshot(request);
            return DurableMessageDisposition::Ack;
        }
        catch (const std::invalid_argument& error) {
            LG_WARN("service=exchange-gateway event=snapshot_request_invalid disposition=terminate error={}", error.what());
            return DurableMessageDisposition::Terminate;
        }
        catch (const std::exception& error) {
            LG_ERROR("service=exchange-gateway event=snapshot_request_failed disposition=retry error={}", error.what());
            return DurableMessageDisposition::Retry;
        }
    }

    DurableMessageDisposition onNotionalPlanDryRun(const BusMessage& message)
    {
        try {
            const NotionalOrderPlanBatch plan =
                ContractJsonCodec::decodeNotionalOrderPlanBatch(message.payload);

            validateMetadata(plan.metadata);

            const Timestamp newestCompleted = newestCompletedUtcDate(time_handler_);
            if (plan.decision_timestamp == 0 || plan.decision_timestamp > newestCompleted) {
                throw std::invalid_argument(
                    "NotionalOrderPlan decision_timestamp is ahead of the newest completed business day"
                );
            }

            LG_INFO(
                "service=exchange-gateway event=hyperliquid_dry_run_plan_received "
                "decision_timestamp={} newest_completed_business_day={} "
                "state_revision={} orders={} message_id={} action=prepare_raw_only",
                plan.decision_timestamp,
                newestCompleted,
                plan.state_revision,
                plan.submit_orders.size(),
                plan.metadata.message_id
            );

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

                LG_INFO(
                    "service=exchange-gateway event=hyperliquid_dry_run_raw_order "
                    "economic_order_id={} order_id={} coin={} side={} "
                    "decision_timestamp={} state_revision={} "
                    "reference_close={} requested_notional_usd={} "
                    "raw_quantity={} venue=hyperliquid "
                    "venue_rules_applied=false submitted=false",
                    order.economic_order_id,
                    order.order_id,
                    order.coin,
                    order.side == OrderSide::Buy ? "buy" : "sell",
                    order.decision_timestamp,
                    order.state_revision,
                    order.reference_close,
                    order.notional_usd,
                    rawQuantity
                );
            }

            return DurableMessageDisposition::Ack;
        }
        catch (const std::invalid_argument& error) {
            LG_WARN(
                "service=exchange-gateway "
                "event=hyperliquid_dry_run_plan_invalid "
                "disposition=terminate error={}",
                error.what()
            );
            return DurableMessageDisposition::Terminate;
        }
        catch (const std::exception& error) {
            LG_ERROR(
                "service=exchange-gateway "
                "event=hyperliquid_dry_run_plan_failed "
                "disposition=retry error={}",
                error.what()
            );
            return DurableMessageDisposition::Retry;
        }
    }

public:
    explicit ExchangeGatewayRuntime(Options options)
        : options_(std::move(options)),
          time_handler_(TimeHandlerFactory::createFromEnvironment()),
          bus_(options_.nats_url)
    {
        // PATCH 24 deliberately keeps snapshot requests on a separate control stream.
        // Existing PATCH 17-23 runtime streams therefore need no in-place subject update.
        // Gateway is bound only to normal trading subjects. Business/event-time
        // validation is local through TimeHandler; no shared-clock control plane is used.
        bus_.ensureStream(
            options_.runtime_stream,
            TransportSubjects::tradingRuntimeSubjects()
        );
        if (options_.mode == GatewayMode::Backend) {
            bus_.ensureStream(
                options_.control_stream,
                TransportSubjects::exchangeGatewayControlSubjects()
            );

            adapter_ = std::make_unique<NatsBackendExchangeGatewayAdapter>(
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
                    TransportSubjects::SUBMIT_ORDER
                ),
                [this](const BusMessage& message) { return onSubmit(message); }
            );

            cancel_subscription_ = bus_.subscribe(
                consumer(
                    options_.runtime_stream,
                    "exchange-gateway-cancel",
                    TransportSubjects::CANCEL_ORDER
                ),
                [this](const BusMessage& message) { return onCancel(message); }
            );

            snapshot_request_subscription_ = bus_.subscribe(
                consumer(
                    options_.control_stream,
                    "exchange-gateway-snapshot-request",
                    TransportSubjects::EXCHANGE_SNAPSHOT_REQUEST
                ),
                [this](const BusMessage& message) {
                    return onSnapshotRequest(message);
                }
            );
        }
        else {
            // STEP 7: this mode can only inspect/prepare notional plans.
            // It deliberately has no SubmitOrder/CancelOrder/backend binding.
            notional_plan_subscription_ = bus_.subscribe(
                consumer(
                    options_.runtime_stream,
                    "exchange-gateway-hyperliquid-dry-run-plan",
                    TransportSubjects::NOTIONAL_ORDER_PLAN
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
        LG_INFO(
            "service=exchange-gateway event=service_ready mode={} runtime_stream={} control_stream={} backend_stream={} business_date={} time_model=local_time_handler poll_timeout_ms={} real_submission=false",
            gatewayModeName(options_.mode),
            options_.runtime_stream,
            options_.control_stream,
            options_.backend_stream,
            currentBusinessUtcDate(time_handler_),
            options_.poll_timeout_ms
        );
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
                // STEP 7 dry-run boundary: no backend and no real submission path.
                bus_.poll(
                    notional_plan_subscription_,
                    32,
                    options_.poll_timeout_ms
                );
            }
        }

        LG_INFO("service=exchange-gateway event=shutdown_requested");
        bus_.flush();
        LG_INFO("service=exchange-gateway event=shutdown_complete");
    }
};

} // namespace


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
        LG_ALERT("service=exchange-gateway event=fatal error={}", error.what());
        return 1;
    }
}
