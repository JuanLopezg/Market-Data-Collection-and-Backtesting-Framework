#pragma once

#include "message_bus.h"
#include "exchange.h"


// Distributed ExecutionEngine outbound exchange-command adapter
//
// Commands are durably published. Inbound OrderUpdate/Fill messages are intentionally
// NOT consumed here: the service host must deliver them to ExecutionEngine::processExchangeEvent
// and ACK only after that call (including persistence) succeeds.
class MessageExchange final : public Exchange {
private:
    MessageBus& bus_;

public:
    explicit MessageExchange(MessageBus& bus)
        : bus_(bus)
    {}

    void submitOrder(const ExecutionOrder& order) override;
    void cancelOrder(OrderID orderId) override;

    std::vector<ExchangeEvent> drainEvents() override
    {
        return {};
    }
};
