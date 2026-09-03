#include "rolling_market_state.h"

#include <cmath>
#include <stdexcept>
#include <unordered_map>
#include <utility>


namespace {

void validateBar(const MarketBarSnapshot& value)
{
    if (value.coin.empty())
        throw std::invalid_argument("Market slice contains an empty coin");

    const OHLCV& bar = value.bar;
    if (!std::isfinite(bar.open) || bar.open <= 0.0 ||
        !std::isfinite(bar.high) || bar.high <= 0.0 ||
        !std::isfinite(bar.low) || bar.low <= 0.0 ||
        !std::isfinite(bar.close) || bar.close <= 0.0 ||
        !std::isfinite(bar.volume) || bar.volume < 0.0)
        throw std::invalid_argument("Market slice contains invalid OHLCV data");
}

bool sameBar(const OHLCV& left, const OHLCV& right)
{
    return left.open == right.open &&
           left.high == right.high &&
           left.low == right.low &&
           left.close == right.close &&
           left.volume == right.volume;
}

PriceSnapshot pricesFor(
    const MarketData& marketData,
    Timestamp timestamp,
    bool useOpen
)
{
    const auto tsIt = marketData.find(timestamp);
    if (tsIt == marketData.end())
        throw std::out_of_range("Market slice timestamp is not available");

    PriceSnapshot prices;
    for (const auto& [coin, bar] : tsIt->second)
        prices.set(coin, useOpen ? bar.open : bar.close);

    return prices;
}

} // namespace


bool RollingMarketState::append(const MarketSliceSnapshot& slice)
{
    if (slice.timestamp == 0)
        throw std::invalid_argument("Market slice timestamp must be non-zero");
    if (slice.bars.empty())
        throw std::invalid_argument("Market slice cannot be empty");
    if (!empty_ && slice.timestamp < latest_timestamp_)
        throw std::logic_error("Out-of-order market slice");

    std::unordered_map<Coin, OHLCV> incoming;
    incoming.reserve(slice.bars.size());

    for (const MarketBarSnapshot& value : slice.bars) {
        validateBar(value);
        if (!incoming.emplace(value.coin, value.bar).second)
            throw std::invalid_argument("Market slice contains duplicate coin");
    }

    if (!empty_ && slice.timestamp == latest_timestamp_) {
        const auto existingIt = market_data_.find(slice.timestamp);
        if (existingIt == market_data_.end() || existingIt->second.size() != incoming.size())
            throw std::logic_error("Conflicting redelivery of latest market slice");

        for (const auto& [coin, ohlcv] : incoming) {
            const auto rawCoinIt = raw_data_.data.find(coin);
            if (rawCoinIt == raw_data_.data.end())
                throw std::logic_error("Conflicting redelivery of latest market slice");

            const auto rawBarIt = rawCoinIt->second.find(slice.timestamp);
            if (rawBarIt == rawCoinIt->second.end() || !sameBar(rawBarIt->second, ohlcv))
                throw std::logic_error("Conflicting redelivery of latest market slice");
        }

        return false;
    }

    CoinBarMap marketBars;
    marketBars.reserve(incoming.size());

    for (const auto& [coin, ohlcv] : incoming) {
        raw_data_.data[coin].emplace(slice.timestamp, ohlcv);

        BarData bar;
        bar.open = ohlcv.open;
        bar.high = ohlcv.high;
        bar.low = ohlcv.low;
        bar.close = ohlcv.close;
        bar.volume = ohlcv.volume;
        bar.barNumber = ++bar_counts_[coin];
        marketBars.emplace(coin, bar);
    }

    market_data_.emplace(slice.timestamp, std::move(marketBars));
    latest_timestamp_ = slice.timestamp;
    empty_ = false;
    return true;
}


PriceSnapshot RollingMarketState::openPrices(Timestamp timestamp) const
{
    return pricesFor(market_data_, timestamp, true);
}


PriceSnapshot RollingMarketState::closePrices(Timestamp timestamp) const
{
    return pricesFor(market_data_, timestamp, false);
}
