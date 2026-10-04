#pragma once

#include "ranker.h"


/**************************************************************************************
 * Type    : IndicatorRanker
 * Purpose : Generic ranker based on one parameterized indicator
 *
 * Examples :
 *
 *   IndicatorRanker(RSI(Close, 14), true)
 *      -> ranks coins by RSI(14), highest first
 *
 *   IndicatorRanker(ROC(Close, 5), false)
 *      -> ranks coins by ROC(5), lowest first
 *
 *   IndicatorRanker(SMA(Volume, 25), true)
 *      -> ranks coins by 25-bar average volume, highest first
 **************************************************************************************/
class IndicatorRanker : public Ranker {
public:
    /**************************************************************************************
     * Purpose : Construct an indicator-based ranker
     *
     * Args :
     *   spec        - indicator used as the ranking score
     *   descending  - true means highest score ranks first
     *                 false means lowest score ranks first
     *   skipInvalid - true means NaN/invalid indicator values are excluded
     **************************************************************************************/
    IndicatorRanker(
        IndicatorSpec spec,
        bool descending = true,
        bool skipInvalid = true
    );

    /**************************************************************************************
     * Purpose : Rank current universe by the configured indicator value
     **************************************************************************************/
    RankedUniverse rank(
        const CoinBarMap& bars,
        Timestamp ts,
        const IndicatorEngine& indicators
    ) const override;

    /**************************************************************************************
     * Purpose : Expose the indicator needed by this ranker
     **************************************************************************************/
    std::vector<IndicatorSpec> requiredIndicators() const override;

    /**************************************************************************************
     * Purpose : Access the indicator specification used by this ranker
     **************************************************************************************/
    const IndicatorSpec& GetSpec() const;

private:
    IndicatorSpec spec_;
    bool descending_;
    bool skipInvalid_;
};