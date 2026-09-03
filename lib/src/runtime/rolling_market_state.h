#pragma once

#include <unordered_map>

#include "market_slice_snapshot.h"
#include "price_snapshot.h"


/**************************************************************************************
 * Type    : RollingMarketState
 * Purpose : Rebuild Decision-local market history from complete slice messages
 *
 * append() is idempotent for an identical redelivery of the latest timestamp and rejects
 * out-of-order/conflicting slices. This gives the future durable consumer a deterministic
 * no-lookahead boundary independent from the transport implementation.
 **************************************************************************************/
class RollingMarketState {
private:
    OHLCVData raw_data_;
    MarketData market_data_;
    std::unordered_map<Coin, unsigned int> bar_counts_;
    Timestamp latest_timestamp_ = 0;
    bool empty_ = true;

public:
    bool append(const MarketSliceSnapshot& slice);

    bool empty() const { return empty_; }
    Timestamp latestTimestamp() const { return latest_timestamp_; }

    const OHLCVData& rawData() const { return raw_data_; }
    const MarketData& marketData() const { return market_data_; }

    PriceSnapshot openPrices(Timestamp timestamp) const;
    PriceSnapshot closePrices(Timestamp timestamp) const;
};
