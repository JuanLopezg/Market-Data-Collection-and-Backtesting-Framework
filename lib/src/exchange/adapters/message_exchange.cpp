// Translate local execution commands into durable messages. Publishing a command
// does not confirm acceptance or create a fill; those arrive through exchange events.

#include "message_exchange.h"

#include <cmath>
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
    // The order fields are public: a malformed order must never reach the wire.
    if (order.coin.empty())
        throw std::invalid_argument("MessageExchange order asset cannot be empty");
    if (!std::isfinite(order.quantity) || order.quantity <= 0.0)
        throw std::invalid_argument("MessageExchange order quantity must be finite and positive");

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
