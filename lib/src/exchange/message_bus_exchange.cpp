#include "message_bus_exchange.h"

#include <stdexcept>
#include <string>

#include "contract_json_codec.h"
#include "execution_commands.h"
#include "transport_subjects.h"


namespace {

std::string orderCorrelationId(OrderID orderId)
{
    return "order-" + std::to_string(orderId);
}

std::string submitMessageId(OrderID orderId)
{
    return "submit-order-" + std::to_string(orderId);
}

std::string cancelMessageId(OrderID orderId)
{
    return "cancel-order-" + std::to_string(orderId);
}

} // namespace


void MessageBusExchange::submitOrder(const ExecutionOrder& order)
{
    if (order.order_id == 0)
        throw std::invalid_argument("MessageBusExchange order id must be non-zero");

    SubmitOrderCommand command;
    command.metadata.message_id = submitMessageId(order.order_id);
    command.metadata.correlation_id = orderCorrelationId(order.order_id);
    command.metadata.produced_at = order.created_at;
    command.order = order;

    bus_.publish(
        TransportSubjects::SUBMIT_ORDER,
        ContractJsonCodec::encode(command),
        command.metadata.message_id
    );
}


void MessageBusExchange::cancelOrder(OrderID orderId)
{
    if (orderId == 0)
        throw std::invalid_argument("MessageBusExchange cancel order id must be non-zero");

    CancelOrderCommand command;
    command.metadata.message_id = cancelMessageId(orderId);
    command.metadata.correlation_id = orderCorrelationId(orderId);
    command.order_id = orderId;

    bus_.publish(
        TransportSubjects::CANCEL_ORDER,
        ContractJsonCodec::encode(command),
        command.metadata.message_id
    );
}
