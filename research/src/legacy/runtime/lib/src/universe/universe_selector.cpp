#include "universe_selector.h"


std::vector<IndicatorSpec> UniverseSelector::requiredIndicators() const
{
    return {};
}


CoinBarMap AllUniverseSelector::select(
    const CoinBarMap& bars,
    Timestamp ts,
    const IndicatorEngine& indicators
) const
{
    (void)ts;
    (void)indicators;

    return bars;
}