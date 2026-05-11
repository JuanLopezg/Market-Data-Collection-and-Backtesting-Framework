#include "liquidity_universe.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "indicator_engine.h"


namespace {

struct ScoredCoin {
    Coin coin;
    const BarData* bar = nullptr;
    double score = 0.0;
};

} // namespace


TopNLiquidityUniverse::TopNLiquidityUniverse(
    IndicatorSpec liquiditySpec,
    unsigned int topN,
    bool descending,
    bool skipInvalid
)
    : liquiditySpec_(liquiditySpec),
      topN_(topN),
      descending_(descending),
      skipInvalid_(skipInvalid)
{}


CoinBarMap TopNLiquidityUniverse::select(
    const CoinBarMap& bars,
    Timestamp ts,
    const IndicatorEngine& indicators
) const
{
    CoinBarMap selected;

    if (topN_ == 0 || bars.empty()) {
        return selected;
    }

    std::vector<ScoredCoin> scored;
    scored.reserve(bars.size());

    for (const auto& [coin, bar] : bars) {
        const double score = indicators.value(
            coin,
            ts,
            liquiditySpec_
        );

        if (skipInvalid_ && !std::isfinite(score)) {
            continue;
        }

        scored.push_back(
            ScoredCoin{
                coin,
                &bar,
                score
            }
        );
    }

    std::sort(
        scored.begin(),
        scored.end(),
        [this](const ScoredCoin& a, const ScoredCoin& b) {
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

    const std::size_t limit = std::min(
        static_cast<std::size_t>(topN_),
        scored.size()
    );

    selected.reserve(limit);

    for (std::size_t i = 0; i < limit; ++i) {
        if (scored[i].bar == nullptr) {
            continue;
        }

        selected.emplace(
            scored[i].coin,
            *scored[i].bar
        );
    }

    return selected;
}


std::vector<IndicatorSpec> TopNLiquidityUniverse::requiredIndicators() const
{
    return { liquiditySpec_ };
}


const IndicatorSpec& TopNLiquidityUniverse::GetLiquiditySpec() const
{
    return liquiditySpec_;
}


unsigned int TopNLiquidityUniverse::GetTopN() const
{
    return topN_;
}