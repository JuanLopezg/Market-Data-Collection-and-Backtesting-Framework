#pragma once

#include <vector>

#include "data_types.h"
#include "indicator_spec.h"


/**************************************************************************************
 * Purpose : Calculate simple moving average
 **************************************************************************************/
std::vector<double> calculateSMA(
    const std::vector<OHLCV>& bars,
    PriceField source,
    unsigned int length
);


/**************************************************************************************
 * Purpose : Calculate exponential moving average
 **************************************************************************************/
std::vector<double> calculateEMA(
    const std::vector<OHLCV>& bars,
    PriceField source,
    unsigned int length
);


/**************************************************************************************
 * Purpose : Calculate rate of change
 *
 * Formula:
 *   ROC[i] = value[i] / value[i - length] - 1
 **************************************************************************************/
std::vector<double> calculateROC(
    const std::vector<OHLCV>& bars,
    PriceField source,
    unsigned int length
);


/**************************************************************************************
 * Purpose : Calculate RSI using Wilder smoothing
 **************************************************************************************/
std::vector<double> calculateRSI(
    const std::vector<OHLCV>& bars,
    PriceField source,
    unsigned int length
);


/**************************************************************************************
 * Purpose : Calculate ATR as simple moving average of true range
 *
 * This follows the style of your current implementation:
 *   ATR(14) first valid value appears at index 14,
 *   using TR[1] through TR[14].
 **************************************************************************************/
std::vector<double> calculateATR(
    const std::vector<OHLCV>& bars,
    unsigned int length
);


/**************************************************************************************
 * Purpose : Calculate rolling highest value
 **************************************************************************************/
std::vector<double> calculateHighest(
    const std::vector<OHLCV>& bars,
    PriceField source,
    unsigned int length
);


/**************************************************************************************
 * Purpose : Calculate rolling lowest value
 **************************************************************************************/
std::vector<double> calculateLowest(
    const std::vector<OHLCV>& bars,
    PriceField source,
    unsigned int length
);


/**************************************************************************************
 * Purpose : Calculate Donchian upper band
 **************************************************************************************/
std::vector<double> calculateDonchianHigh(
    const std::vector<OHLCV>& bars,
    unsigned int length
);


/**************************************************************************************
 * Purpose : Calculate Donchian lower band
 **************************************************************************************/
std::vector<double> calculateDonchianLow(
    const std::vector<OHLCV>& bars,
    unsigned int length
);


/**************************************************************************************
 * Purpose : Calculate Donchian midpoint
 **************************************************************************************/
std::vector<double> calculateDonchianMid(
    const std::vector<OHLCV>& bars,
    unsigned int length
);
