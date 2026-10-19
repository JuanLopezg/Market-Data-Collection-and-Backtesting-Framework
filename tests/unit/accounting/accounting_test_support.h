#pragma once

#include <utility>

#include "account.h"
#include "position_ledger.h"
#include "trade_recorder.h"

namespace accounting_test {

inline constexpr Timestamp kD1 = 20260105;
inline constexpr Timestamp kD2 = 20260106;
inline constexpr Timestamp kD3 = 20260107;
inline constexpr Timestamp kD4 = 20260108;
inline constexpr StrategyID kAlpha = 7;
inline constexpr StrategyID kBeta = 9;

inline Fill fill(FillID id, OrderSide side, double quantity, double price,
                 double fee = 0.0, Timestamp at = kD1,
                 StrategyID strategy = kAlpha, Coin coin = "BTC")
{
    Fill result;
    result.fill_id = id;
    result.order_id = static_cast<OrderID>(id);
    result.strategy_id = strategy;
    result.timestamp = at;
    result.coin = std::move(coin);
    result.side = side;
    result.quantity = quantity;
    result.price = price;
    result.commission = fee;
    return result;
}

inline PriceSnapshot marks(double btc, double eth = 20.0)
{
    PriceSnapshot result;
    result.set("BTC", btc);
    result.set("ETH", eth);
    return result;
}

inline void averageEntry(PositionLedger& ledger)
{
    ledger.applyFill(fill(101, OrderSide::Buy, 2.0, 100.0, 2.0));
    ledger.applyFill(fill(102, OrderSide::Buy, 2.0, 200.0, 4.0, kD2));
}

inline void averageEntry(TradeRecorder& recorder)
{
    recorder.onFill(fill(101, OrderSide::Buy, 2.0, 100.0, 2.0), "Alpha");
    recorder.onFill(fill(102, OrderSide::Buy, 2.0, 200.0, 4.0, kD2), "Alpha");
}

} // namespace accounting_test
