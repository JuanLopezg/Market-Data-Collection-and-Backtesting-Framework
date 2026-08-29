#include "strategy.h"

#include <utility>
#include <vector>

#include "no_ranker.h"


Strategy::Strategy(
    std::string name,
    unsigned int maxActiveSignals,
    std::unique_ptr<UniverseSelector> universeSelector,
    std::unique_ptr<Ranker> ranker,
    unsigned int maxRankingPosition,
    std::vector<std::unique_ptr<MarketFilter>> marketFilters
)
    : strategy_name_(std::move(name)),
      maxActiveSignals_(maxActiveSignals),
      universeSelector_(std::move(universeSelector)),
      ranker_(std::move(ranker)),
      marketFilters_(std::move(marketFilters)),
      maxRankingPosition_(maxRankingPosition)
{
    if (!universeSelector_)
        universeSelector_ = std::make_unique<AllUniverseSelector>();

    if (!ranker_)
        ranker_ = std::make_unique<NoRanker>();
}


std::vector<IndicatorSpec> Strategy::requiredIndicators() const
{
    std::vector<IndicatorSpec> specs;

    for (const auto& filter : marketFilters_) {
        if (!filter)
            continue;

        const auto filterSpecs = filter->requiredIndicators();
        specs.insert(specs.end(), filterSpecs.begin(), filterSpecs.end());
    }

    if (universeSelector_) {
        const auto universeSpecs = universeSelector_->requiredIndicators();
        specs.insert(specs.end(), universeSpecs.begin(), universeSpecs.end());
    }

    if (ranker_) {
        const auto rankerSpecs = ranker_->requiredIndicators();
        specs.insert(specs.end(), rankerSpecs.begin(), rankerSpecs.end());
    }

    return specs;
}


bool Strategy::marketFiltersPass(
    const MarketData& marketData,
    Timestamp ts,
    const IndicatorEngine& indicators
) const
{
    for (const auto& filter : marketFilters_) {
        if (filter && !filter->passes(marketData, ts, indicators))
            return false;
    }

    return true;
}
