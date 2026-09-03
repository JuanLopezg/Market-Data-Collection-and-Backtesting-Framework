#include <algorithm>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "account.h"
#include "contract_json_codec.h"
#include "exchange_snapshot_event.h"
#include "exchange_snapshot_request.h"
#include "execution_commands.h"
#include "execution_events.h"
#include "execution_price_snapshot.h"
#include "nats_jetstream_message_bus.h"
#include "service_logging.h"
#include "simulated_exchange.h"
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
    std::string backend_stream = "ALGOTRADING_EXCHANGE_BACKEND";
    double initial_cash = 100000.0;
    double commission_rate = 0.0;
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
        else if (arg == "--backend-stream")
            options.backend_stream = requireValue("--backend-stream");
        else if (arg == "--initial-cash")
            options.initial_cash = std::stod(requireValue("--initial-cash"));
        else if (arg == "--commission-rate")
            options.commission_rate = std::stod(requireValue("--commission-rate"));
        else if (arg == "--poll-timeout-ms")
            options.poll_timeout_ms = std::stoi(requireValue("--poll-timeout-ms"));
        else if (arg == "--help" || arg == "-h") {
            std::cout
                << "Simulated exchange service options:\n"
                << "  --nats-url URL\n"
                << "  --stream NAME\n"
                << "  --backend-stream NAME\n"
                << "  --initial-cash VALUE\n"
                << "  --commission-rate VALUE\n"
                << "  --poll-timeout-ms N\n";
            std::exit(0);
        }
        else
            throw std::invalid_argument("Unknown option: " + arg);
    }

    if (options.runtime_stream.empty() || options.backend_stream.empty())
        throw std::invalid_argument("Simulated-exchange stream names cannot be empty");
    if (!std::isfinite(options.initial_cash) || options.initial_cash <= 0.0)
        throw std::invalid_argument("--initial-cash must be finite and positive");
    if (!std::isfinite(options.commission_rate) || options.commission_rate < 0.0)
        throw std::invalid_argument("--commission-rate must be finite and non-negative");
    if (options.poll_timeout_ms <= 0)
        throw std::invalid_argument("--poll-timeout-ms must be positive");

    return options;
}


bool sameOrder(const ExecutionOrder& lhs, const ExecutionOrder& rhs)
{
    return lhs.order_id == rhs.order_id &&
           lhs.strategy_id == rhs.strategy_id &&
           lhs.created_at == rhs.created_at &&
           lhs.active_from == rhs.active_from &&
           lhs.coin == rhs.coin &&
           lhs.side == rhs.side &&
           lhs.quantity == rhs.quantity;
}


bool samePrices(
    const std::unordered_map<Coin, double>& lhs,
    const std::unordered_map<Coin, double>& rhs
)
{
    return lhs == rhs;
}


CoinBarMap openBars(const std::unordered_map<Coin, double>& prices)
{
    CoinBarMap bars;
    bars.reserve(prices.size());

    for (const auto& [coin, price] : prices) {
        if (!std::isfinite(price) || price <= 0.0)
            throw std::invalid_argument("Simulated execution price must be finite and positive");

        BarData bar;
        bar.open = price;
        bars.emplace(coin, bar);
    }

    return bars;
}


std::string orderUpdateMessageId(const OrderUpdate& update)
{
    return "sim-order-update:" + std::to_string(update.order_id) + ":" +
           std::to_string(static_cast<int>(update.status)) + ":" +
           std::to_string(update.timestamp);
}


std::string fillMessageId(const Fill& fill)
{
    return "sim-fill:" + std::to_string(fill.fill_id);
}


class SimulatedExchangeServiceRuntime {
private:
    const Options options_;
    NatsJetStreamMessageBus bus_;
    SimulatedExchange exchange_;
    Account account_;

    std::map<Timestamp, std::unordered_map<Coin, double>> execution_prices_;
    std::unordered_map<OrderID, ExecutionOrder> known_orders_;
    std::vector<ExchangeEvent> pending_events_;
    Timestamp latest_timestamp_ = 0;

    DurableMessageBus::SubscriptionID prices_subscription_ = 0;
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
            throw std::invalid_argument("Invalid simulated-exchange contract metadata");
    }

    ContractMetadata eventMetadata(
        std::string messageId,
        std::string correlationId,
        Timestamp timestamp
    ) const
    {
        ContractMetadata result;
        result.schema_version = 1;
        result.message_id = std::move(messageId);
        result.correlation_id = std::move(correlationId);
        result.produced_at = timestamp;
        return result;
    }

    void captureExchangeEvents()
    {
        std::vector<ExchangeEvent> generated = exchange_.drainEvents();
        for (ExchangeEvent& event : generated) {
            if (const auto* fill = std::get_if<Fill>(&event))
                account_.applyFill(*fill);
            pending_events_.push_back(std::move(event));
        }
    }

    void flushPendingEvents(const std::string& correlationId)
    {
        std::size_t published = 0;

        while (published < pending_events_.size()) {
            const ExchangeEvent& event = pending_events_[published];

            if (const auto* update = std::get_if<OrderUpdate>(&event)) {
                OrderUpdateEvent output;
                output.metadata = eventMetadata(
                    orderUpdateMessageId(*update),
                    correlationId,
                    update->timestamp
                );
                output.update = *update;
                bus_.publish(
                    TransportSubjects::BACKEND_ORDER_UPDATE,
                    ContractJsonCodec::encode(output),
                    output.metadata.message_id
                );
                LG_INFO(
                    "service=simulated-exchange event=order_update_published order_id={} timestamp={} status={} message_id={} correlation_id={}",
                    output.update.order_id,
                    output.update.timestamp,
                    static_cast<int>(output.update.status),
                    output.metadata.message_id,
                    output.metadata.correlation_id
                );
            }
            else if (const auto* fill = std::get_if<Fill>(&event)) {
                FillEvent output;
                output.metadata = eventMetadata(
                    fillMessageId(*fill),
                    correlationId,
                    fill->timestamp
                );
                output.fill = *fill;
                bus_.publish(
                    TransportSubjects::BACKEND_FILL,
                    ContractJsonCodec::encode(output),
                    output.metadata.message_id
                );
                LG_INFO(
                    "service=simulated-exchange event=fill_published fill_id={} order_id={} coin={} side={} quantity={} price={} commission={} message_id={} correlation_id={}",
                    output.fill.fill_id,
                    output.fill.order_id,
                    output.fill.coin,
                    output.fill.side == OrderSide::Buy ? "buy" : "sell",
                    output.fill.quantity,
                    output.fill.price,
                    output.fill.commission,
                    output.metadata.message_id,
                    output.metadata.correlation_id
                );
            }

            ++published;
        }

        if (published != 0)
            pending_events_.erase(pending_events_.begin(), pending_events_.begin() + published);
    }

    void processOpenIfAvailable(const ExecutionOrder& order)
    {
        const auto priceIt = execution_prices_.find(order.active_from);
        if (priceIt == execution_prices_.end())
            return;

        exchange_.processOpen(order.active_from, openBars(priceIt->second));
        captureExchangeEvents();
    }

    ExchangeSnapshot snapshot() const
    {
        ExchangeSnapshot result;
        result.timestamp = std::max<Timestamp>(1, latest_timestamp_);
        result.cash = account_.cash();
        result.positions = account_.positions().values();

        result.open_orders.reserve(exchange_.activeOrders().size());
        for (const auto& [orderId, order] : exchange_.activeOrders()) {
            ExchangeOpenOrderSnapshot open;
            open.local_order_id = orderId;
            open.exchange_order_id = "sim-" + std::to_string(orderId);
            open.coin = order.coin;
            open.side = order.side;
            open.quantity = order.quantity;
            open.filled_quantity = 0.0;
            result.open_orders.push_back(std::move(open));
        }

        std::sort(
            result.open_orders.begin(),
            result.open_orders.end(),
            [](const ExchangeOpenOrderSnapshot& lhs, const ExchangeOpenOrderSnapshot& rhs) {
                return lhs.local_order_id < rhs.local_order_id;
            }
        );
        return result;
    }

    DurableMessageDisposition onPrices(const BusMessage& message)
    {
        try {
            const ExecutionPriceSnapshot prices =
                ContractJsonCodec::decodeExecutionPriceSnapshot(message.payload);
            validateMetadata(prices.metadata);
            if (prices.timestamp == 0 || prices.prices.empty())
                return DurableMessageDisposition::Terminate;

            LG_INFO(
                "service=simulated-exchange event=execution_prices_received decision_timestamp={} execution_timestamp={} prices={} message_id={}",
                prices.decision_timestamp,
                prices.timestamp,
                prices.prices.size(),
                prices.metadata.message_id
            );

            const auto existing = execution_prices_.find(prices.timestamp);
            if (existing != execution_prices_.end()) {
                if (!samePrices(existing->second, prices.prices))
                    return DurableMessageDisposition::Terminate;
                flushPendingEvents(prices.metadata.message_id);
                return DurableMessageDisposition::Ack;
            }

            if (latest_timestamp_ != 0 && prices.timestamp < latest_timestamp_)
                return DurableMessageDisposition::Terminate;

            execution_prices_.emplace(prices.timestamp, prices.prices);
            latest_timestamp_ = std::max(latest_timestamp_, prices.timestamp);

            // Fill any already-active orders at this exact simulated open.
            exchange_.processOpen(prices.timestamp, openBars(prices.prices));
            captureExchangeEvents();
            flushPendingEvents(prices.metadata.message_id);
            LG_INFO(
                "service=simulated-exchange event=execution_open_processed execution_timestamp={} cash={} positions={} active_orders={} known_orders={}",
                prices.timestamp,
                account_.cash(),
                account_.positions().values().size(),
                exchange_.activeOrders().size(),
                known_orders_.size()
            );
            return DurableMessageDisposition::Ack;
        }
        catch (const std::invalid_argument& error) {
            LG_WARN("service=simulated-exchange event=execution_prices_invalid disposition=terminate error={}", error.what());
            return DurableMessageDisposition::Terminate;
        }
        catch (const std::exception& error) {
            LG_ERROR("service=simulated-exchange event=execution_prices_failed disposition=retry error={}", error.what());
            return DurableMessageDisposition::Retry;
        }
    }

    DurableMessageDisposition onSubmit(const BusMessage& message)
    {
        try {
            const SubmitOrderCommand command =
                ContractJsonCodec::decodeSubmitOrderCommand(message.payload);
            validateMetadata(command.metadata);
            if (command.order.order_id == 0)
                return DurableMessageDisposition::Terminate;

            LG_INFO(
                "service=simulated-exchange event=submit_received order_id={} strategy_id={} coin={} side={} quantity={} active_from={} message_id={}",
                command.order.order_id,
                command.order.strategy_id,
                command.order.coin,
                command.order.side == OrderSide::Buy ? "buy" : "sell",
                command.order.quantity,
                command.order.active_from,
                command.metadata.message_id
            );

            const auto known = known_orders_.find(command.order.order_id);
            if (known != known_orders_.end()) {
                if (!sameOrder(known->second, command.order)) {
                    LG_ALERT(
                        "service=simulated-exchange event=submit_conflict order_id={} action=terminate",
                        command.order.order_id
                    );
                    return DurableMessageDisposition::Terminate;
                }
                LG_INFO(
                    "service=simulated-exchange event=submit_duplicate order_id={} disposition=ack",
                    command.order.order_id
                );
                flushPendingEvents(command.metadata.message_id);
                return DurableMessageDisposition::Ack;
            }

            known_orders_.emplace(command.order.order_id, command.order);
            exchange_.submitOrder(command.order);
            captureExchangeEvents();

            // In distributed replay the T+1 open commonly arrives before the resulting
            // SubmitOrder command. Use the order's exact active_from price, never a newer
            // "latest" price, so transport latency cannot change backtest semantics.
            processOpenIfAvailable(command.order);
            flushPendingEvents(command.metadata.message_id);
            LG_INFO(
                "service=simulated-exchange event=submit_processed order_id={} cash={} positions={} active_orders={} known_orders={}",
                command.order.order_id,
                account_.cash(),
                account_.positions().values().size(),
                exchange_.activeOrders().size(),
                known_orders_.size()
            );
            return DurableMessageDisposition::Ack;
        }
        catch (const std::invalid_argument& error) {
            LG_WARN("service=simulated-exchange event=submit_invalid disposition=terminate error={}", error.what());
            return DurableMessageDisposition::Terminate;
        }
        catch (const std::exception& error) {
            LG_ERROR("service=simulated-exchange event=submit_failed disposition=retry error={}", error.what());
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
                "service=simulated-exchange event=cancel_received order_id={} requested_at={} message_id={}",
                command.order_id,
                command.requested_at,
                command.metadata.message_id
            );
            exchange_.cancelOrderAt(command.order_id, command.requested_at);
            captureExchangeEvents();
            flushPendingEvents(command.metadata.message_id);
            return DurableMessageDisposition::Ack;
        }
        catch (const std::invalid_argument& error) {
            LG_WARN("service=simulated-exchange event=cancel_invalid disposition=terminate error={}", error.what());
            return DurableMessageDisposition::Terminate;
        }
        catch (const std::exception& error) {
            LG_ERROR("service=simulated-exchange event=cancel_failed disposition=retry error={}", error.what());
            return DurableMessageDisposition::Retry;
        }
    }

    DurableMessageDisposition onSnapshotRequest(const BusMessage& message)
    {
        try {
            const ExchangeSnapshotRequest request =
                ContractJsonCodec::decodeExchangeSnapshotRequest(message.payload);
            validateMetadata(request.metadata);

            flushPendingEvents(request.metadata.message_id);

            ExchangeSnapshotEvent output;
            output.snapshot = snapshot();
            output.metadata = eventMetadata(
                "sim-snapshot:" + request.metadata.message_id,
                request.metadata.message_id,
                output.snapshot.timestamp
            );
            bus_.publish(
                TransportSubjects::BACKEND_EXCHANGE_SNAPSHOT,
                ContractJsonCodec::encode(output),
                output.metadata.message_id
            );
            LG_INFO(
                "service=simulated-exchange event=snapshot_published timestamp={} cash={} positions={} open_orders={} message_id={} correlation_id={}",
                output.snapshot.timestamp,
                output.snapshot.cash,
                output.snapshot.positions.size(),
                output.snapshot.open_orders.size(),
                output.metadata.message_id,
                output.metadata.correlation_id
            );
            return DurableMessageDisposition::Ack;
        }
        catch (const std::invalid_argument& error) {
            LG_WARN("service=simulated-exchange event=snapshot_invalid disposition=terminate error={}", error.what());
            return DurableMessageDisposition::Terminate;
        }
        catch (const std::exception& error) {
            LG_ERROR("service=simulated-exchange event=snapshot_failed disposition=retry error={}", error.what());
            return DurableMessageDisposition::Retry;
        }
    }

public:
    explicit SimulatedExchangeServiceRuntime(Options options)
        : options_(std::move(options)),
          bus_(options_.nats_url),
          exchange_(options_.commission_rate),
          account_(options_.initial_cash)
    {
        bus_.ensureStream(options_.runtime_stream, TransportSubjects::runtimeSubjects());
        bus_.ensureStream(options_.backend_stream, TransportSubjects::exchangeBackendSubjects());

        prices_subscription_ = bus_.subscribe(
            consumer(
                options_.runtime_stream,
                "simulated-exchange-prices",
                TransportSubjects::EXECUTION_PRICES
            ),
            [this](const BusMessage& message) { return onPrices(message); }
        );
        submit_subscription_ = bus_.subscribe(
            consumer(
                options_.backend_stream,
                "simulated-exchange-submit",
                TransportSubjects::BACKEND_SUBMIT_ORDER
            ),
            [this](const BusMessage& message) { return onSubmit(message); }
        );
        cancel_subscription_ = bus_.subscribe(
            consumer(
                options_.backend_stream,
                "simulated-exchange-cancel",
                TransportSubjects::BACKEND_CANCEL_ORDER
            ),
            [this](const BusMessage& message) { return onCancel(message); }
        );
        snapshot_request_subscription_ = bus_.subscribe(
            consumer(
                options_.backend_stream,
                "simulated-exchange-snapshot-request",
                TransportSubjects::BACKEND_EXCHANGE_SNAPSHOT_REQUEST
            ),
            [this](const BusMessage& message) { return onSnapshotRequest(message); }
        );
    }

    ~SimulatedExchangeServiceRuntime()
    {
        bus_.close(snapshot_request_subscription_);
        bus_.close(cancel_subscription_);
        bus_.close(submit_subscription_);
        bus_.close(prices_subscription_);
    }

    void run()
    {
        LG_INFO(
            "service=simulated-exchange event=service_ready runtime_stream={} backend_stream={} initial_cash={} commission_rate={} poll_timeout_ms={}",
            options_.runtime_stream,
            options_.backend_stream,
            options_.initial_cash,
            options_.commission_rate,
            options_.poll_timeout_ms
        );

        while (running.load()) {
            // Prices first: orders submitted later for the same active_from timestamp are
            // still filled from the stored exact open by onSubmit().
            bus_.poll(prices_subscription_, 32, options_.poll_timeout_ms);
            bus_.poll(snapshot_request_subscription_, 8, options_.poll_timeout_ms);
            bus_.poll(cancel_subscription_, 32, options_.poll_timeout_ms);
            bus_.poll(submit_subscription_, 32, options_.poll_timeout_ms);
        }

        LG_INFO("service=simulated-exchange event=shutdown_requested pending_events={}", pending_events_.size());
        flushPendingEvents("simulated-exchange-shutdown");
        bus_.flush();
        LG_INFO("service=simulated-exchange event=shutdown_complete");
    }
};

} // namespace


int main(int argc, char** argv)
{
    ServiceLogging::setup("simulated-exchange");

    try {
        std::signal(SIGINT, stopHandler);
        std::signal(SIGTERM, stopHandler);
        SimulatedExchangeServiceRuntime runtime(parseOptions(argc, argv));
        runtime.run();
        return 0;
    }
    catch (const std::exception& error) {
        LG_ALERT("service=simulated-exchange event=fatal error={}", error.what());
        return 1;
    }
}
