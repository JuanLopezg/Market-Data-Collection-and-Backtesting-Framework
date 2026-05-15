#include "indicator_engine.h"

#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "indicator_calculators.h"


namespace {

double invalidValue()
{
    return std::numeric_limits<double>::quiet_NaN();
}


std::vector<double> invalidVector(std::size_t size)
{
    return std::vector<double>(size, invalidValue());
}


std::vector<double> applyOffset(
    const std::vector<double>& values,
    unsigned int offset
)
{
    if (offset == 0) {
        return values;
    }

    std::vector<double> shifted(values.size(), invalidValue());
    const std::size_t off = static_cast<std::size_t>(offset);

    if (off >= values.size()) {
        return shifted;
    }

    for (std::size_t i = off; i < values.size(); ++i) {
        shifted[i] = values[i - off];
    }

    return shifted;
}

} // namespace


void IndicatorEngine::precompute(
    const OHLCVData& rawData,
    const std::vector<IndicatorSpec>& specs
)
{
    cache_.clear();
    timestampToIndexByCoin_.clear();

    std::unordered_set<IndicatorSpec, IndicatorSpecHash> uniqueSpecs;

    for (const auto& spec : specs) {
        uniqueSpecs.insert(spec);
    }

    std::unordered_map<Coin, CoinSeries> seriesByCoin;
    seriesByCoin.reserve(rawData.data.size());

    for (const auto& [coin, series] : rawData.data) {
        CoinSeries coinSeries;

        coinSeries.timestamps.reserve(series.size());
        coinSeries.bars.reserve(series.size());

        auto& timestampToIndex = timestampToIndexByCoin_[coin];
        timestampToIndex.reserve(series.size());

        std::size_t index = 0;

        for (const auto& [ts, ohlcv] : series) {
            coinSeries.timestamps.push_back(ts);
            coinSeries.bars.push_back(ohlcv);
            timestampToIndex.emplace(ts, index);

            ++index;
        }

        seriesByCoin.emplace(coin, std::move(coinSeries));
    }

    for (const auto& spec : uniqueSpecs) {
        std::unordered_map<Coin, std::vector<double>> valuesByCoin;
        valuesByCoin.reserve(seriesByCoin.size());

        for (const auto& [coin, series] : seriesByCoin) {
            valuesByCoin.emplace(
                coin,
                computeForCoin(series, spec)
            );
        }

        cache_.emplace(spec, std::move(valuesByCoin));
    }
}


double IndicatorEngine::value(
    const Coin& coin,
    Timestamp ts,
    const IndicatorSpec& spec
) const
{
    const auto coinIndexIt = timestampToIndexByCoin_.find(coin);

    if (coinIndexIt == timestampToIndexByCoin_.end()) {
        return invalidValue();
    }

    const auto indexIt = coinIndexIt->second.find(ts);

    if (indexIt == coinIndexIt->second.end()) {
        return invalidValue();
    }

    const std::size_t index = indexIt->second;

    const auto specIt = cache_.find(spec);

    if (specIt == cache_.end()) {
        return invalidValue();
    }

    const auto coinIt = specIt->second.find(coin);

    if (coinIt == specIt->second.end()) {
        return invalidValue();
    }

    const auto& values = coinIt->second;

    if (index >= values.size()) {
        return invalidValue();
    }

    return values[index];
}


bool IndicatorEngine::has(
    const Coin& coin,
    Timestamp ts,
    const IndicatorSpec& spec
) const
{
    const double v = value(coin, ts, spec);
    return v == v; // NaN Check
}


std::vector<double> IndicatorEngine::computeForCoin(
    const CoinSeries& series,
    const IndicatorSpec& spec
) const
{
    std::vector<double> values;

    switch (spec.kind) {
        case IndicatorKind::SMA:
            values = calculateSMA(
                series.bars,
                spec.source,
                spec.length
            );
            break;

        case IndicatorKind::EMA:
            values = calculateEMA(
                series.bars,
                spec.source,
                spec.length
            );
            break;

        case IndicatorKind::RSI:
            values = calculateRSI(
                series.bars,
                spec.source,
                spec.length
            );
            break;

        case IndicatorKind::ATR:
            values = calculateATR(
                series.bars,
                spec.length
            );
            break;

        case IndicatorKind::ROC:
            values = calculateROC(
                series.bars,
                spec.source,
                spec.length
            );
            break;

        case IndicatorKind::Highest:
            values = calculateHighest(
                series.bars,
                spec.source,
                spec.length
            );
            break;

        case IndicatorKind::Lowest:
            values = calculateLowest(
                series.bars,
                spec.source,
                spec.length
            );
            break;

        default:
            values = invalidVector(series.bars.size());
            break;
    }

    return applyOffset(values, spec.offset);
}