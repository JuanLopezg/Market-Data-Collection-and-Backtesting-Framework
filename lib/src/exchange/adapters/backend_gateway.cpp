#include "backend_gateway.h"

#include <stdexcept>
#include <utility>

#include "message_json.h"
#include "message_subjects.h"


DurableConsumerOptions BackendGateway::consumer(
    const std::string& durable,
    const std::string& subject
) const
{
    DurableConsumerOptions result;
    result.stream = stream_;
    result.durable_name = durable;
    result.subject = subject;
    result.ack_wait_ms = 30000;
    result.max_deliver = 20;
    result.max_ack_pending = 256;
    return result;
}


BackendGateway::BackendGateway(
    const std::string& natsUrl,
    std::string stream
)
    : stream_(std::move(stream)),
      bus_(natsUrl)
{
    if (stream_.empty())
        throw std::invalid_argument("Exchange backend stream cannot be empty");

    bus_.ensureStream(stream_, MessageSubjects::exchangeBackendSubjects());

    // OrderUpdate and Fill share one ordered consumer. Using separate filtered
    // consumers would allow Accepted/Fill/Filled to be observed out of stream order.
    event_subscription_ = bus_.subscribe(
        consumer("exchange-gateway-backend-events", "gateway.backend.event.>"),
        [this](const BusMessage& message) {
            try {
                if (message.subject == MessageSubjects::BACKEND_ORDER_UPDATE) {
                    if (!handlers_.on_order_update)
                        return DurableMessageDisposition::Retry;
                    const OrderUpdateEvent value =
                        MessageJson::decodeOrderUpdateEvent(message.payload);
                    // Defer events beyond the allowed replay frontier without losing them.
                    if (event_time_gate_ && !event_time_gate_(value.metadata.produced_at))
                        return DurableMessageDisposition::Retry;
                    handlers_.on_order_update(value);
                    return DurableMessageDisposition::Ack;
                }

                if (message.subject == MessageSubjects::BACKEND_FILL) {
                    if (!handlers_.on_fill)
                        return DurableMessageDisposition::Retry;
                    const FillEvent value = MessageJson::decodeFillEvent(message.payload);
                    // Defer events beyond the allowed replay frontier without losing them.
                    if (event_time_gate_ && !event_time_gate_(value.metadata.produced_at))
                        return DurableMessageDisposition::Retry;
                    handlers_.on_fill(value);
                    return DurableMessageDisposition::Ack;
                }

                return DurableMessageDisposition::Terminate;
            }
            catch (...) {
                return DurableMessageDisposition::Retry;
            }
        }
    );

    snapshot_subscription_ = bus_.subscribe(
        consumer("exchange-gateway-backend-snapshots", MessageSubjects::BACKEND_EXCHANGE_SNAPSHOT),
        [this](const BusMessage& message) {
            try {
                if (!handlers_.on_snapshot)
                    return DurableMessageDisposition::Retry;
                const ExchangeSnapshotEvent value =
                    MessageJson::decodeExchangeSnapshotEvent(message.payload);
                if (event_time_gate_ && !event_time_gate_(value.metadata.produced_at))
                    return DurableMessageDisposition::Retry;
                handlers_.on_snapshot(value);
                return DurableMessageDisposition::Ack;
            }
            catch (...) {
                return DurableMessageDisposition::Retry;
            }
        }
    );
}


BackendGateway::~BackendGateway()
{
    bus_.close(snapshot_subscription_);
    bus_.close(event_subscription_);
}


void BackendGateway::setHandlers(ExchangeGatewayHandlers handlers)
{
    if (!handlers.on_order_update || !handlers.on_fill || !handlers.on_snapshot)
        throw std::invalid_argument("Exchange gateway adapter requires all event handlers");
    handlers_ = std::move(handlers);
}


void BackendGateway::setEventTimeGate(std::function<bool(Timestamp)> gate)
{
    event_time_gate_ = std::move(gate);
}


void BackendGateway::submitOrder(const SubmitOrderCommand& command)
{
    if (command.metadata.message_id.empty() || command.order.order_id == 0)
        throw std::invalid_argument("Invalid gateway submit command");

    bus_.publish(
        MessageSubjects::BACKEND_SUBMIT_ORDER,
        MessageJson::encode(command),
        command.metadata.message_id
    );
}


void BackendGateway::cancelOrder(const CancelOrderCommand& command)
{
    if (command.metadata.message_id.empty() || command.order_id == 0)
        throw std::invalid_argument("Invalid gateway cancel command");

    bus_.publish(
        MessageSubjects::BACKEND_CANCEL_ORDER,
        MessageJson::encode(command),
        command.metadata.message_id
    );
}


void BackendGateway::requestSnapshot(
    const ExchangeSnapshotRequest& request
)
{
    if (request.metadata.message_id.empty())
        throw std::invalid_argument("Invalid gateway snapshot request");

    bus_.publish(
        MessageSubjects::BACKEND_EXCHANGE_SNAPSHOT_REQUEST,
        MessageJson::encode(request),
        request.metadata.message_id
    );
}


void BackendGateway::poll(int timeoutMs)
{
    if (timeoutMs <= 0)
        throw std::invalid_argument("Gateway adapter poll timeout must be positive");

    bus_.poll(event_subscription_, 64, timeoutMs);
    bus_.poll(snapshot_subscription_, 8, timeoutMs);
}
