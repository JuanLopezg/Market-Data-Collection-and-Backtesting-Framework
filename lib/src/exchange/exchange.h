#pragma once

#include <vector>

#include "exchange_event.h"
#include "execution_order.h"


/**************************************************************************************
 * Type    : Exchange
 * Purpose : Abstract execution boundary shared by simulated and live trading
 *
 * submitOrder/cancelOrder are commands. Exchange responses are emitted later as one
 * ordered stream of OrderUpdate/Fill events, matching asynchronous live APIs.
 **************************************************************************************/
class Exchange {
public:
    virtual ~Exchange() = default;

    virtual void submitOrder(const ExecutionOrder& order) = 0;
    virtual void cancelOrder(OrderID orderId) = 0;
    virtual std::vector<ExchangeEvent> drainEvents() = 0;
};
