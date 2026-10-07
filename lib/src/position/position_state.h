#pragma once

#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <unordered_map>
#include "data_types.h"

// One quantity container; the aliases distinguish requested targets from actually filled strategy positions.

using PositionQuantity = double;


// Generic quantity state by asset
//
// This representation is intentionally independent of strategy, backtest and live
// execution. It can be used for actual account positions, strategy attribution or
// desired account targets.
//
// Missing assets are treated as quantity 0.
class PositionState {
private:
    std::unordered_map<Coin, PositionQuantity> quantities_;

public:
    PositionQuantity get(const Coin& coin) const
    {
        const auto it = quantities_.find(coin);
        return it == quantities_.end() ? 0.0 : it->second;
    }

    void set(const Coin& coin, PositionQuantity quantity)
    {
        if (!std::isfinite(quantity))
            throw std::invalid_argument("Position quantity must be finite");

        if (quantity == 0.0) {
            quantities_.erase(coin);
            return;
        }

        quantities_[coin] = quantity;
    }

    void add(const Coin& coin, PositionQuantity quantityChange)
    {
        if (!std::isfinite(quantityChange))
            throw std::invalid_argument("Position quantity change must be finite");

        set(coin, get(coin) + quantityChange);
    }

    bool contains(const Coin& coin) const
    {
        return quantities_.find(coin) != quantities_.end();
    }

    std::size_t size() const
    {
        return quantities_.size();
    }

    const std::unordered_map<Coin, PositionQuantity>& values() const
    {
        return quantities_;
    }

    void clear()
    {
        quantities_.clear();
    }
};

// Desired quantity by asset before execution
//
// It can represent one strategy target or the final net account target depending on
// where it is used.
using TargetPositionState = PositionState;

// Filled quantities attributed internally to one strategy
//
// This is strategy-level accounting state, not a separate exchange position. Several
// strategies may therefore hold opposing virtual quantities while the real account only
// owns their net quantity.
//
// It must be updated from fills/internal allocation, never merely because a strategy
// requested a new target.
using VirtualPositionState = PositionState;

// Execution-owned filled quantities exposed read-only to decision logic
//
// Decision may inspect this snapshot when applying rebalance policy, but only Execution
// mutates the authoritative quantities in response to actual Fill events.
using StrategyPositionSnapshot = std::unordered_map<StrategyID, VirtualPositionState>;
