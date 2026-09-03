#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

#include "contract_json_codec.h"
#include "exchange_gateway_adapter.h"
#include "exchange_snapshot_request.h"
#include "nats_backend_exchange_gateway_adapter.h"
#include "nats_jetstream_message_bus.h"
#include "service_logging.h"
#include "transport_subjects.h"


namespace {

std::atomic<bool> running{true};

void stopHandler(int)
{
    running.store(false);
}


struct Options {
    std::string nats_url = "nats://127.0.0.1:4222";
    std::string runtime_stream = "ALGOTRADING_RUNTIME";
    std::string control_stream = "ALGOTRADING_EXCHANGE_CONTROL";
    std::string backend_stream = "ALGOTRADING_EXCHANGE_BACKEND";
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
        else if (arg == "--poll-timeout-ms")
            options.poll_timeout_ms = std::stoi(requireValue("--poll-timeout-ms"));
        else if (arg == "--help" || arg == "-h") {
            std::cout
                << "Exchange gateway options:\n"
                << "  --nats-url URL\n"
                << "  --stream NAME\n"
                << "  --control-stream NAME\n"
                << "  --backend-stream NAME\n"
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
    NatsJetStreamMessageBus bus_;
    NatsBackendExchangeGatewayAdapter adapter_;

    DurableMessageBus::SubscriptionID submit_subscription_ = 0;
    DurableMessageBus::SubscriptionID cancel_subscription_ = 0;
    DurableMessageBus::SubscriptionID snapshot_request_subscription_ = 0;

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

    void publish(const OrderUpdateEvent& value)
    {
        validateMetadata(value.metadata);
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
    }

    void publish(const ExchangeSnapshotEvent& value)
    {
        validateMetadata(value.metadata);
        if (value.snapshot.timestamp == 0)
            throw std::invalid_argument("Exchange snapshot timestamp must be non-zero");
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
            adapter_.submitOrder(command);
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
            adapter_.cancelOrder(command);
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
            adapter_.requestSnapshot(request);
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

public:
    explicit ExchangeGatewayRuntime(Options options)
        : options_(std::move(options)),
          bus_(options_.nats_url),
          adapter_(options_.nats_url, options_.backend_stream)
    {
        // PATCH 24 deliberately keeps snapshot requests on a separate control stream.
        // Existing PATCH 17-23 runtime streams therefore need no in-place subject update.
        bus_.ensureStream(options_.runtime_stream, TransportSubjects::runtimeSubjects());
        bus_.ensureStream(
            options_.control_stream,
            TransportSubjects::exchangeGatewayControlSubjects()
        );

        adapter_.setHandlers({
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
            [this](const BusMessage& message) { return onSnapshotRequest(message); }
        );
    }

    ~ExchangeGatewayRuntime()
    {
        bus_.close(snapshot_request_subscription_);
        bus_.close(cancel_subscription_);
        bus_.close(submit_subscription_);
    }

    void run()
    {
        LG_INFO(
            "service=exchange-gateway event=service_ready runtime_stream={} control_stream={} backend_stream={} backend=nats-service poll_timeout_ms={}",
            options_.runtime_stream,
            options_.control_stream,
            options_.backend_stream,
            options_.poll_timeout_ms
        );

        while (running.load()) {
            // Exchange/backend events first so downstream state observes exchange truth
            // before additional outbound commands are forwarded whenever both are ready.
            adapter_.poll(options_.poll_timeout_ms);
            bus_.poll(snapshot_request_subscription_, 8, options_.poll_timeout_ms);
            bus_.poll(cancel_subscription_, 32, options_.poll_timeout_ms);
            bus_.poll(submit_subscription_, 32, options_.poll_timeout_ms);
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
