#include "data_types.h"


/**************************************************************************************
 * Purpose : Build market data from raw OHLCV data
 *
 * This function reorganizes the raw OHLCV data from:
 *
 *   coin -> timestamp -> OHLCV
 *
 * into:
 *
 *   timestamp -> coin -> BarData
 *
 * It intentionally does not calculate any indicators.
 * Indicators are now handled separately by IndicatorEngine.
 **************************************************************************************/
MarketData buildMarketData(const OHLCVData& raw)
{
    MarketData marketData;

    for (const auto& [coin, series] : raw.data)
    {
        unsigned int barNumber = 1;

        for (const auto& [ts, ohlcv] : series)
        {
            BarData bar;

            bar.open      = ohlcv.open;
            bar.high      = ohlcv.high;
            bar.low       = ohlcv.low;
            bar.close     = ohlcv.close;
            bar.volume    = ohlcv.volume;
            bar.barNumber = barNumber;

            marketData[ts][coin] = bar;

            ++barNumber;
        }
    }

    return marketData;
}


/**************************************************************************************
 * Purpose : Compatibility wrapper for previous code
 *
 * Older code used buildEnriched().
 * The new architecture no longer enriches BarData with indicators, so this simply
 * delegates to buildMarketData().
 **************************************************************************************/
EnrichedData buildEnriched(const OHLCVData& raw)
{
    return buildMarketData(raw);
}