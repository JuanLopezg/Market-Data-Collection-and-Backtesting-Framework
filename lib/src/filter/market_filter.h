#pragma once

#include <vector>

#include "data_types.h"
#include "indicator_spec.h"


class IndicatorEngine;


// Base class for reusable market/regime filters
//
// A MarketFilter decides whether strategy entries are allowed at a timestamp.
//
// Example:
// - BTC close > BTC SMA50
// - ETH close > ETH SMA200
// - total market regime filter
class MarketFilter {
public:
    virtual ~MarketFilter() = default;

    // Check whether this market filter passes at the current timestamp
    virtual bool passes(
        const MarketData& marketData,
        Timestamp ts,
        const IndicatorEngine& indicators
    ) const = 0;

    // Return indicators required by this filter
    virtual std::vector<IndicatorSpec> requiredIndicators() const { return {}; }
};