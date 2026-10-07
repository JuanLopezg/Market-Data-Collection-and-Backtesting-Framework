// Translate local execution commands into durable messages. Publishing a command
// does not confirm acceptance or create a fill; those arrive through exchange events.

#include "message_exchange.h"

#include <stdexcept>
#include <string>

#include "message_json.h"
#include "execution_messages.h"
#include "message_subjects.h"


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


void MessageExchange::submitOrder(const ExecutionOrder& order)
{
    if (order.order_id == 0)
        throw std::invalid_argument("MessageExchange order id must be non-zero");

    SubmitOrderCommand command;
    command.metadata.message_id = submitMessageId(order.order_id);
    command.metadata.correlation_id = orderCorrelationId(order.order_id);
    command.metadata.produced_at = order.created_at;
    command.order = order;

    bus_.publish(
        MessageSubjects::SUBMIT_ORDER,
        MessageJson::encode(command),
        command.metadata.message_id
    );
}


void MessageExchange::cancelOrder(OrderID orderId)
{
    if (orderId == 0)
        throw std::invalid_argument("MessageExchange cancel order id must be non-zero");

    CancelOrderCommand command;
    command.metadata.message_id = cancelMessageId(orderId);
    command.metadata.correlation_id = orderCorrelationId(orderId);
    command.order_id = orderId;

    bus_.publish(
        MessageSubjects::CANCEL_ORDER,
        MessageJson::encode(command),
        command.metadata.message_id
    );
}
