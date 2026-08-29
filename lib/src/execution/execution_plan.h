#pragma once

#include <vector>

#include "execution_order.h"


/**************************************************************************************
 * Type    : ExecutionPlan
 * Purpose : Exchange commands required to move current/pending state toward a target
 *
 * Cancellations are deliberately separate from submissions. If an existing open order
 * conflicts with a newer target, cancel it first and wait for the exchange confirmation
 * before creating its replacement. This avoids crossed/duplicate live orders.
 **************************************************************************************/
struct ExecutionPlan {
    std::vector<OrderID> order_ids_to_cancel;
    std::vector<ExecutionOrder> orders_to_submit;

    bool empty() const
    {
        return order_ids_to_cancel.empty() && orders_to_submit.empty();
    }
};
