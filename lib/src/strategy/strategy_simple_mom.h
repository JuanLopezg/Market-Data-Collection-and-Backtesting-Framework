#include "strategy.h"

#include <algorithm>

/*
 * Simple momentum strategy:
 * - Enter long if close > previous close
 * - Exit if momentum turns negative
 */
class StrategySimpleMomentum : public Strategy {
public:
    inline void calculateSignals(
        std::vector<Trade>& current_trades, const CoinBarMap& bars, Timestamp ts) override {

        for (const auto& [coin, bar] : bars) {

            
        }
    }
};
