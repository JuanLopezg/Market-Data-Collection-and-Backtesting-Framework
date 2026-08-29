#pragma once

#include <algorithm>
#include <string>
#include <utility>

#include "execution_order.h"
#include "execution_order_status.h"


/**************************************************************************************
 * Type    : TrackedOrder
 * Purpose : Mutable lifecycle state for one immutable ExecutionOrder command
 **************************************************************************************/
struct TrackedOrder {
    ExecutionOrder request;
    ExecutionOrderStatus status = ExecutionOrderStatus::Created;

    double filled_quantity = 0.0;
    Timestamp updated_at = 0;
    bool cancel_requested = false;

    std::string exchange_order_id;
    std::string last_message;

    TrackedOrder() = default;

    explicit TrackedOrder(ExecutionOrder executionOrder)
        : request(std::move(executionOrder)),
          updated_at(request.created_at)
    {}

    double remainingQuantity() const
    {
        return std::max(0.0, request.quantity - filled_quantity);
    }

    double pendingSignedQuantity() const
    {
        if (isTerminalExecutionOrderStatus(status))
            return 0.0;

        const double remaining = remainingQuantity();
        return request.side == OrderSide::Buy ? remaining : -remaining;
    }

    bool isOpen() const
    {
        return !isTerminalExecutionOrderStatus(status);
    }
};
