#pragma once

#include <cstddef>
#include <unordered_map>
#include <vector>

#include "data_types.h"
#include "indicator_spec.h"


/**************************************************************************************
 * Type    : IndicatorEngine
 * Purpose : Caches parameterized indicator values
 *
 * The engine allows strategies, rankers, and universe selectors to request values like:
 *
 *   RSI(Close, 14) for BTC at timestamp X
 *   ATR(14)        for ETH at timestamp Y
 *   ROC(Close, 5)  for SOL at timestamp Z
 *
 * Indicator values should be precomputed before the backtest loop starts.
 *
 * Important design:
 *   IndicatorEngine does NOT permanently store raw OHLCVData.
 *   It only receives raw data during precompute(), then stores:
 *     - timestamp -> vector index maps
 *     - computed indicator values
 **************************************************************************************/
class IndicatorEngine {
public:
    IndicatorEngine() = default;

    /**************************************************************************************
     * Purpose : Precompute all requested indicators for all coins
     **************************************************************************************/
    void precompute(
        const OHLCVData& rawData,
        const std::vector<IndicatorSpec>& specs
    );

    /**************************************************************************************
     * Purpose : Return one indicator value for coin + timestamp + spec
     *
     * Return : NaN if the value is unavailable, not precomputed, or out of range
     **************************************************************************************/
    double value(
        const Coin& coin,
        Timestamp ts,
        const IndicatorSpec& spec
    ) const;

    /**************************************************************************************
     * Purpose : Check whether a precomputed value exists for coin + timestamp + spec
     **************************************************************************************/
    bool has(
        const Coin& coin,
        Timestamp ts,
        const IndicatorSpec& spec
    ) const;

private:
    struct CoinSeries {
        std::vector<Timestamp> timestamps;
        std::vector<OHLCV> bars;
    };

    std::unordered_map<
        Coin,
        std::unordered_map<Timestamp, std::size_t>
    > timestampToIndexByCoin_;

    std::unordered_map<
        IndicatorSpec,
        std::unordered_map<Coin, std::vector<double>>,
        IndicatorSpecHash
    > cache_;

    std::vector<double> computeForCoin(
        const CoinSeries& series,
        const IndicatorSpec& spec
    ) const;
};