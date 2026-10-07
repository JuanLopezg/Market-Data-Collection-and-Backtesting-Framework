#pragma once

#include <vector>

#include "data_types.h"
#include "indicator_spec.h"


// Calculate simple moving average
std::vector<double> calculateSMA(
    const std::vector<OHLCV>& bars,
    PriceField source,
    unsigned int length
);


// Calculate exponential moving average
std::vector<double> calculateEMA(
    const std::vector<OHLCV>& bars,
    PriceField source,
    unsigned int length
);


// Calculate rate of change
//
// Formula:
// ROC[i] = value[i] / value[i - length] - 1
std::vector<double> calculateROC(
    const std::vector<OHLCV>& bars,
    PriceField source,
    unsigned int length
);


// Calculate RSI using Wilder smoothing
std::vector<double> calculateRSI(
    const std::vector<OHLCV>& bars,
    PriceField source,
    unsigned int length
);


// Calculate ATR as simple moving average of true range
//
// This follows the style of your current implementation:
// ATR(14) first valid value appears at index 14,
// using TR[1] through TR[14].
std::vector<double> calculateATR(
    const std::vector<OHLCV>& bars,
    unsigned int length
);


// Calculate rolling highest value
std::vector<double> calculateHighest(
    const std::vector<OHLCV>& bars,
    PriceField source,
    unsigned int length
);


// Calculate rolling lowest value
std::vector<double> calculateLowest(
    const std::vector<OHLCV>& bars,
    PriceField source,
    unsigned int length
);


// Calculate Donchian upper band
std::vector<double> calculateDonchianHigh(
    const std::vector<OHLCV>& bars,
    unsigned int length
);


// Calculate Donchian lower band
std::vector<double> calculateDonchianLow(
    const std::vector<OHLCV>& bars,
    unsigned int length
);


// Calculate Donchian midpoint
std::vector<double> calculateDonchianMid(
    const std::vector<OHLCV>& bars,
    unsigned int length
);
