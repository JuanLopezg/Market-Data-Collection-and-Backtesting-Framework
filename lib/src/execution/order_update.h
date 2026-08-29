#pragma once

#include <stdexcept>
#include <string>
#include <utility>

#include "data_types.h"
#include "execution_order_status.h"


/**************************************************************************************
 * Type    : OrderUpdate
 * Purpose : Exchange-originated lifecycle update, separate from actual Fill events
 *
 * exchange_order_id is intentionally a string because real exchanges do not share one
 * identifier format. message may contain a reject/cancel reason for diagnostics.
 **************************************************************************************/
struct OrderUpdate {
    OrderID order_id = 0;
    Timestamp timestamp = 0;
    ExecutionOrderStatus status = ExecutionOrderStatus::Created;
    std::string exchange_order_id;
    std::string message;

    OrderUpdate() = default;

    OrderUpdate(
        OrderID orderId,
        Timestamp updateTimestamp,
        ExecutionOrderStatus orderStatus,
        std::string exchangeOrderId = {},
        std::string updateMessage = {}
    )
        : order_id(orderId),
          timestamp(updateTimestamp),
          status(orderStatus),
          exchange_order_id(std::move(exchangeOrderId)),
          message(std::move(updateMessage))
    {
        if (order_id == 0)
            throw std::invalid_argument("Order update id must be non-zero");
    }
};
