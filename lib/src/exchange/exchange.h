#pragma once

#include <vector>

#include <variant>

#include "fill.h"
#include "execution_order.h"


// Ordered event stream emitted by simulated/fake/live exchange adapters
//
// One ordered stream avoids losing event order when ACKs, partial fills and final order
// status updates arrive asynchronously.
using ExchangeEvent = std::variant<OrderUpdate, Fill>;

// Abstract execution boundary shared by simulated and live trading
//
// submitOrder/cancelOrder are commands. Exchange responses are emitted later as one
// ordered stream of OrderUpdate/Fill events, matching asynchronous live APIs.
class Exchange {
public:
    virtual ~Exchange() = default;

    virtual void submitOrder(const ExecutionOrder& order) = 0;
    virtual void cancelOrder(OrderID orderId) = 0;
    virtual std::vector<ExchangeEvent> drainEvents() = 0;
};
