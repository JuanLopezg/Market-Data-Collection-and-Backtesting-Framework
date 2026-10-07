#pragma once

#include <algorithm>
#include <vector>

#include "data_types.h"
#include "indicator_engine.h"
#include "indicator_spec.h"
#include "ranker.h"


// Deterministic alphabetical ordering without an indicator score
//
// It simply converts the selected universe into a RankedUniverse.
//
// Important:
// CoinBarMap is an unordered_map, so iteration order is not naturally stable.
// To keep backtests deterministic, this ranker sorts coins alphabetically.
class AlphabeticalRanker : public Ranker {
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
            ranked.emplace_back(
                RankedCoin{
                    coin,
                    &bar,
                    0.0
                }
            );
        }

        std::sort(
            ranked.begin(),
            ranked.end(),
            [](const RankedCoin& left, const RankedCoin& right) {
                return left.coin < right.coin;
            }
        );

        return ranked;
    }

    std::vector<IndicatorSpec> requiredIndicators() const override
    {
        return {};
    }
};