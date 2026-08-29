#pragma once

#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <unordered_map>

#include "data_types.h"


using SignalValue = double;


/**************************************************************************************
 * Type    : SignalState
 * Purpose : Stores the current signal produced by one strategy for each coin
 *
 * Signals are continuous values in the range [-1, +1]:
 *   +1.0 = full long signal
 *    0.0 = no active opinion
 *   -1.0 = full short signal
 *
 * Intermediate values such as +0.5 or -0.25 are also valid.
 * Coins with signal 0 are not stored internally; a missing coin therefore means 0.
 **************************************************************************************/
class SignalState {
private:
    std::unordered_map<Coin, SignalValue> signals_;

public:
    /**************************************************************************************
     * Purpose : Return the current signal for a coin
     * Note    : Coins not present in the state are treated as signal 0
     **************************************************************************************/
    SignalValue get(const Coin& coin) const
    {
        const auto it = signals_.find(coin);
        return it == signals_.end() ? 0.0 : it->second;
    }

    /**************************************************************************************
     * Purpose : Set the current signal for a coin
     * Note    : Signal values must stay inside [-1, +1]. Setting 0 removes the coin
     *           from the internal map because missing coins already represent signal 0
     **************************************************************************************/
    void set(const Coin& coin, SignalValue value)
    {
        if (!std::isfinite(value) || value < -1.0 || value > 1.0)
            throw std::out_of_range("Signal must be between -1 and +1");

        if (value == 0.0) {
            signals_.erase(coin);
            return;
        }

        signals_[coin] = value;
    }

    /**************************************************************************************
     * Purpose : Check whether a coin currently has a non-zero signal
     **************************************************************************************/
    bool isActive(const Coin& coin) const
    {
        return signals_.find(coin) != signals_.end();
    }

    /**************************************************************************************
     * Purpose : Number of coins that currently have a non-zero signal
     **************************************************************************************/
    std::size_t activeCount() const
    {
        return signals_.size();
    }

    /**************************************************************************************
     * Purpose : Read all currently active signals
     * Note    : Exposes a const reference so callers cannot bypass set() validation
     **************************************************************************************/
    const std::unordered_map<Coin, SignalValue>& values() const
    {
        return signals_;
    }
};
