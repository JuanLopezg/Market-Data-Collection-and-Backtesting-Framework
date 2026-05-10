
#include "ranker.h"
#include <algorithm>


/**************************************************************************************
 * Purpose : Rank bars based on their traded volume
 * Args    : bars - map of coins to their associated OHLCV bar data
 * Return  : Vector of references to bars ranked by 25 mean volume (descending)
**************************************************************************************/
RankedBars VolumeRanker::rank(const CoinBarMap& bars) const {
    RankedBars ranked;
    ranked.reserve(bars.size());

    for (const auto& kv : bars) {
        ranked.emplace_back(kv);
    }

    std::sort(ranked.begin(), ranked.end(),
        [](const auto& a, const auto& b) {
            return a.get().second.u25d_volume > b.get().second.u25d_volume;
        });

    return ranked;
}


RankedBars ROCRanker::rank(const CoinBarMap& bars) const {
    RankedBars ranked;
    ranked.reserve(bars.size());

    for (const auto& kv : bars) {
        ranked.emplace_back(kv);
    }

    std::sort(ranked.begin(), ranked.end(),
        [](const auto& a, const auto& b) {
            return a.get().second.roc1 > b.get().second.roc1;
        });

    return ranked;
}

RankedBars ROCRankerI::rank(const CoinBarMap& bars) const {
    RankedBars ranked;
    ranked.reserve(bars.size());

    for (const auto& kv : bars) {
        ranked.emplace_back(kv);
    }

    std::sort(ranked.begin(), ranked.end(),
        [](const auto& a, const auto& b) {
            return a.get().second.roc1 < b.get().second.roc1;
        });

    return ranked;
}