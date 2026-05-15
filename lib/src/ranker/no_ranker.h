#pragma once

#include <algorithm>
#include <vector>

#include "data_types.h"
#include "indicator_engine.h"
#include "indicator_spec.h"
#include "ranker.h"


/**************************************************************************************
 * Type    : NoRanker
 * Purpose : Ranker that does not apply any indicator-based ranking
 *
 * It simply converts the selected universe into a RankedUniverse.
 *
 * Important:
 *   CoinBarMap is an unordered_map, so iteration order is not naturally stable.
 *   To keep backtests deterministic, this ranker sorts coins alphabetically.
 **************************************************************************************/
class NoRanker : public Ranker {
public:
    RankedUniverse rank(
        const CoinBarMap& bars,
        Timestamp ts,
        const IndicatorEngine& indicators
    ) const override
    {
        (void)ts;
        (void)indicators;

        RankedUniverse ranked;
        ranked.reserve(bars.size());

        for (const auto& [coin, bar] : bars) {
            (void)bar;
            ranked.emplace_back(RankedCoin{coin, 0.0});
        }

        return ranked;
    }

    std::vector<IndicatorSpec> requiredIndicators() const override
    {
        return {};
    }
};