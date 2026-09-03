#include "nats_backend_exchange_gateway_adapter.h"

#include <stdexcept>
#include <utility>

#include "contract_json_codec.h"
#include "transport_subjects.h"


DurableConsumerOptions NatsBackendExchangeGatewayAdapter::consumer(
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


NatsBackendExchangeGatewayAdapter::NatsBackendExchangeGatewayAdapter(
    const std::string& natsUrl,
    std::string stream
)
    : stream_(std::move(stream)),
      bus_(natsUrl)
{
    if (stream_.empty())
        throw std::invalid_argument("Exchange backend stream cannot be empty");

    bus_.ensureStream(stream_, TransportSubjects::exchangeBackendSubjects());

    // OrderUpdate and Fill share one ordered consumer. Using separate filtered
    // consumers would allow Accepted/Fill/Filled to be observed out of stream order.
    event_subscription_ = bus_.subscribe(
        consumer("exchange-gateway-backend-events", "gateway.backend.event.>"),
        [this](const BusMessage& message) {
            try {
                if (message.subject == TransportSubjects::BACKEND_ORDER_UPDATE) {
                    if (!handlers_.on_order_update)
                        return DurableMessageDisposition::Retry;
                    handlers_.on_order_update(
                        ContractJsonCodec::decodeOrderUpdateEvent(message.payload)
                    );
                    return DurableMessageDisposition::Ack;
                }

                if (message.subject == TransportSubjects::BACKEND_FILL) {
                    if (!handlers_.on_fill)
                        return DurableMessageDisposition::Retry;
                    handlers_.on_fill(ContractJsonCodec::decodeFillEvent(message.payload));
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
        consumer("exchange-gateway-backend-snapshots", TransportSubjects::BACKEND_EXCHANGE_SNAPSHOT),
        [this](const BusMessage& message) {
            try {
                if (!handlers_.on_snapshot)
                    return DurableMessageDisposition::Retry;
                handlers_.on_snapshot(
                    ContractJsonCodec::decodeExchangeSnapshotEvent(message.payload)
                );
                return DurableMessageDisposition::Ack;
            }
            catch (...) {
                return DurableMessageDisposition::Retry;
            }
        }
    );
}


NatsBackendExchangeGatewayAdapter::~NatsBackendExchangeGatewayAdapter()
{
    bus_.close(snapshot_subscription_);
    bus_.close(event_subscription_);
}


void NatsBackendExchangeGatewayAdapter::setHandlers(ExchangeGatewayHandlers handlers)
{
    if (!handlers.on_order_update || !handlers.on_fill || !handlers.on_snapshot)
        throw std::invalid_argument("Exchange gateway adapter requires all event handlers");
    handlers_ = std::move(handlers);
}


void NatsBackendExchangeGatewayAdapter::submitOrder(const SubmitOrderCommand& command)
{
    if (command.metadata.message_id.empty() || command.order.order_id == 0)
        throw std::invalid_argument("Invalid gateway submit command");

    bus_.publish(
        TransportSubjects::BACKEND_SUBMIT_ORDER,
        ContractJsonCodec::encode(command),
        command.metadata.message_id
    );
}


void NatsBackendExchangeGatewayAdapter::cancelOrder(const CancelOrderCommand& command)
{
    if (command.metadata.message_id.empty() || command.order_id == 0)
        throw std::invalid_argument("Invalid gateway cancel command");

    bus_.publish(
        TransportSubjects::BACKEND_CANCEL_ORDER,
        ContractJsonCodec::encode(command),
        command.metadata.message_id
    );
}


void NatsBackendExchangeGatewayAdapter::requestSnapshot(
    const ExchangeSnapshotRequest& request
)
{
    if (request.metadata.message_id.empty())
        throw std::invalid_argument("Invalid gateway snapshot request");

    bus_.publish(
        TransportSubjects::BACKEND_EXCHANGE_SNAPSHOT_REQUEST,
        ContractJsonCodec::encode(request),
        request.metadata.message_id
    );
}


void NatsBackendExchangeGatewayAdapter::poll(int timeoutMs)
{
    if (timeoutMs <= 0)
        throw std::invalid_argument("Gateway adapter poll timeout must be positive");

    bus_.poll(event_subscription_, 64, timeoutMs);
    bus_.poll(snapshot_subscription_, 8, timeoutMs);
}
