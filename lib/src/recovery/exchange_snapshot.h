#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "data_types.h"
#include "execution_order.h"


/**************************************************************************************
 * Type    : ExchangeOpenOrderSnapshot
 * Purpose : Normalized open-order state reported by an external exchange
 *
 * local_order_id is the id encoded by our future client-order-id mapping when available.
 * exchange_order_id remains a string because exchanges use different identifier formats.
 **************************************************************************************/
struct ExchangeOpenOrderSnapshot {
    OrderID local_order_id = 0;
    std::string exchange_order_id;
    Coin coin;
    OrderSide side = OrderSide::Buy;
    double quantity = 0.0;
    double filled_quantity = 0.0;
};


/**************************************************************************************
 * Type    : ExchangeSnapshot
 * Purpose : Exchange-side account/order truth used during startup reconciliation
 *
 * This is deliberately plain data so a future exchange adapter, RPC service or test fake
 * can all produce exactly the same normalized structure.
 **************************************************************************************/
struct ExchangeSnapshot {
    Timestamp timestamp = 0;
    double cash = 0.0;
    std::unordered_map<Coin, double> positions;
    std::vector<ExchangeOpenOrderSnapshot> open_orders;
};
