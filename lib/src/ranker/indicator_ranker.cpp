#include "indicator_ranker.h"

#include <algorithm>
#include <cmath>

#include "indicator_engine.h"


IndicatorRanker::IndicatorRanker(
    IndicatorSpec spec,
    bool descending,
    bool skipInvalid
)
    : spec_(spec),
      descending_(descending),
      skipInvalid_(skipInvalid)
{}


RankedUniverse IndicatorRanker::rank(
    const CoinBarMap& bars,
    Timestamp ts,
    const IndicatorEngine& indicators
) const
{
    RankedUniverse ranked;
    ranked.reserve(bars.size());

    for (const auto& [coin, bar] : bars) {
        const double score = indicators.value(coin, ts, spec_);

        if (skipInvalid_ && !std::isfinite(score)) {
            continue;
        }

        ranked.push_back(
            RankedCoin{
                coin,
                &bar,
                score,
                0
            }
        );
    }

    std::sort(
        ranked.begin(),
        ranked.end(),
        [this](const RankedCoin& a, const RankedCoin& b) {
            const bool aValid = std::isfinite(a.score);
            const bool bValid = std::isfinite(b.score);

            if (aValid != bValid) {
                return aValid;
            }

            if (a.score == b.score) {
                return a.coin < b.coin;
            }

            return descending_
                ? a.score > b.score
                : a.score < b.score;
        }
    );

    for (std::size_t i = 0; i < ranked.size(); ++i) {
        ranked[i].rank = static_cast<unsigned int>(i + 1);
    }

    return ranked;
}


std::vector<IndicatorSpec> IndicatorRanker::requiredIndicators() const
{
    return { spec_ };
}


const IndicatorSpec& IndicatorRanker::GetSpec() const
{
    return spec_;
}