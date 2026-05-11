#pragma once

#include "universe_selector.h"


/**************************************************************************************
 * Type    : TopNLiquidityUniverse
 * Purpose : Select the top N most liquid coins according to a liquidity indicator
 *
 * Example:
 *
 *   TopNLiquidityUniverse(
 *       IndicatorSpec{IndicatorKind::SMA, PriceField::Volume, 25},
 *       20
 *   )
 *
 * This keeps only the 20 coins with the highest SMA(Volume, 25).
 *
 * This class does NOT rank entries by trading signal.
 * It only filters the tradable universe.
 *
 * Typical pipeline:
 *
 *   TopNLiquidityUniverse:
 *      keep top 20 by SMA(Volume, 25)
 *
 *   IndicatorRanker:
 *      rank those 20 by ROC(Close, 1), ascending
 *
 *   Strategy:
 *      enter if fall percentage and MA conditions are met
 **************************************************************************************/
class TopNLiquidityUniverse : public UniverseSelector {
public:
    /**************************************************************************************
     * Purpose : Construct a top-N liquidity selector
     *
     * Args:
     *   liquiditySpec - indicator used as liquidity score
     *   topN          - number of coins to keep
     *   descending    - true means highest liquidity scores are selected
     *   skipInvalid   - true means NaN/invalid liquidity values are excluded
     **************************************************************************************/
    TopNLiquidityUniverse(
        IndicatorSpec liquiditySpec,
        unsigned int topN,
        bool descending = true,
        bool skipInvalid = true
    );

    /**************************************************************************************
     * Purpose : Select the top N coins by liquidity score
     **************************************************************************************/
    CoinBarMap select(
        const CoinBarMap& bars,
        Timestamp ts,
        const IndicatorEngine& indicators
    ) const override;

    /**************************************************************************************
     * Purpose : Expose liquidity indicator required by this selector
     **************************************************************************************/
    std::vector<IndicatorSpec> requiredIndicators() const override;

    const IndicatorSpec& GetLiquiditySpec() const;

    unsigned int GetTopN() const;

private:
    IndicatorSpec liquiditySpec_;
    unsigned int topN_ = 0;
    bool descending_ = true;
    bool skipInvalid_ = true;
};