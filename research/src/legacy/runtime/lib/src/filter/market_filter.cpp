#include "market_filter.h"


MarketFilter::~MarketFilter() = default;


std::vector<IndicatorSpec> MarketFilter::requiredIndicators() const
{
    return {};
}