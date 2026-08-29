#pragma once

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "fill.h"
#include "price_snapshot.h"
#include "trade_record.h"


/**************************************************************************************
 * Type    : TradeRecorder
 * Purpose : Build analytics trade campaigns from strategy-attributed fills
 *
 * A campaign starts when a strategy/asset virtual position leaves zero and ends when it
 * returns to zero. Rebalancing fills inside that interval remain part of the same trade.
 **************************************************************************************/
class TradeRecorder {
private:
    struct OpenTrade {
        TradeRecord record;
        double net_quantity = 0.0;
        double cash_flow = 0.0;
        double entry_notional = 0.0;
        double entry_quantity = 0.0;
        double exit_notional = 0.0;
        double exit_quantity = 0.0;
    };

    using Key = std::pair<StrategyID, Coin>;

    TradeID next_trade_id_ = 1;
    std::map<Key, OpenTrade> open_;
    std::vector<TradeRecord> closed_;
    double cumulative_closed_pnl_ = 0.0;

    static bool sameSign(double a, double b)
    {
        return (a > 0.0 && b > 0.0) || (a < 0.0 && b < 0.0);
    }

    void openTrade(
        const Fill& fill,
        const std::string& strategyName,
        double signedQuantity,
        double commission
    )
    {
        const Key key{fill.strategy_id, fill.coin};
        OpenTrade state;

        state.record.trade_id = next_trade_id_++;
        state.record.strategy_id = fill.strategy_id;
        state.record.strategy_name = strategyName;
        state.record.coin = fill.coin;
        state.record.direction = signedQuantity > 0.0 ? Direction::Long : Direction::Short;
        state.record.start = fill.timestamp;
        state.record.end = fill.timestamp;
        state.record.fill_count = 1;
        state.record.commission = commission;
        // Entry price is the first fill that opens the campaign. Later sizing/rebalance
        // fills may increase the position, but they must not rewrite the original entry.
        state.record.entry_price = fill.price;

        state.net_quantity = signedQuantity;
        state.cash_flow = -signedQuantity * fill.price;
        state.entry_quantity = std::abs(signedQuantity);
        state.entry_notional = state.entry_quantity * fill.price;
        state.record.peak_quantity = state.entry_quantity;

        open_[key] = std::move(state);
    }

    void finalize(const Key& key, Timestamp end, double exitPrice)
    {
        auto it = open_.find(key);
        if (it == open_.end())
            return;

        OpenTrade& state = it->second;
        state.record.end = end;
        // Exit price is the final fill that closes the campaign. Partial reductions and
        // rebalance sells remain part of the trade but must not rewrite the final exit.
        state.record.exit_price = exitPrice;
        state.record.pnl = state.cash_flow - state.record.commission;
        state.record.exited = true;

        // Backtest realized balance changes only when the complete trade campaign closes.
        cumulative_closed_pnl_ += state.record.pnl;
        closed_.push_back(state.record);
        open_.erase(it);
    }

public:
    // Analytics are rebuildable from persisted fills; operational state is not.
    void clear()
    {
        next_trade_id_ = 1;
        open_.clear();
        closed_.clear();
        cumulative_closed_pnl_ = 0.0;
    }

    void onFill(const Fill& fill, const std::string& strategyName)
    {
        fill.validate();

        double remaining = fill.signedQuantity();
        double remainingCommission = fill.commission;
        const Key key{fill.strategy_id, fill.coin};

        while (std::abs(remaining) > 1e-15) {
            auto it = open_.find(key);
            if (it == open_.end()) {
                openTrade(fill, strategyName, remaining, remainingCommission);
                return;
            }

            OpenTrade& state = it->second;
            state.record.end = fill.timestamp;

            if (sameSign(state.net_quantity, remaining)) {
                const double quantity = std::abs(remaining);
                state.cash_flow -= remaining * fill.price;
                state.entry_quantity += quantity;
                state.entry_notional += quantity * fill.price;
                state.record.commission += remainingCommission;
                state.record.fill_count += 1;
                state.net_quantity += remaining;
                state.record.peak_quantity = std::max(
                    state.record.peak_quantity,
                    std::abs(state.net_quantity)
                );
                return;
            }

            const double closeQuantity = std::min(
                std::abs(remaining),
                std::abs(state.net_quantity)
            );
            const double fillQuantity = std::abs(fill.signedQuantity());
            const double closeCommission = fillQuantity > 0.0
                ? fill.commission * (closeQuantity / fillQuantity) : 0.0;
            const double closeSigned = remaining > 0.0 ? closeQuantity : -closeQuantity;

            state.cash_flow -= closeSigned * fill.price;
            state.exit_quantity += closeQuantity;
            state.exit_notional += closeQuantity * fill.price;
            state.record.commission += closeCommission;
            state.record.fill_count += 1;
            state.net_quantity += closeSigned;

            remaining -= closeSigned;
            remainingCommission = std::max(0.0, remainingCommission - closeCommission);

            if (std::abs(state.net_quantity) <= 1e-15)
                finalize(key, fill.timestamp, fill.price);
        }
    }

    const std::vector<TradeRecord>& closedTrades() const
    {
        return closed_;
    }

    // Net PnL from fully closed trade campaigns only. Open trades and partial
    // rebalances do not affect this value until the virtual position returns to zero.
    double cumulativeClosedPnl() const
    {
        return cumulative_closed_pnl_;
    }

    std::vector<TradeRecord> allTrades(
        const PriceSnapshot& marks,
        Timestamp timestamp
    ) const
    {
        std::vector<TradeRecord> result = closed_;

        for (const auto& [key, state] : open_) {
            (void)key;
            if (!marks.contains(state.record.coin))
                throw std::runtime_error("Missing mark price for open trade record");

            TradeRecord record = state.record;
            const double mark = marks.get(record.coin);
            record.end = timestamp;
            record.exit_price = mark;
            record.pnl = state.cash_flow + state.net_quantity * mark - record.commission;
            record.exited = false;
            result.push_back(std::move(record));
        }

        return result;
    }
};
