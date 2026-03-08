#include "data_types.h"

#include <iostream>

/**************************************************************************************
 * Purpose : Build enriched bar data with indicators from raw OHLCV market data
 *
 * This function processes OHLCV time series for each asset and computes additional
 * derived fields used in backtesting. For every bar it calculates:
 *
 *   - 20-day rolling high (previous 20 bars)
 *   - ATR(14) using the previous 14 true ranges
 *
 * Args    : raw - OHLCVData containing raw market candles organized as
 *                 coin -> date -> OHLCV
 *
 * Return  : EnrichedData containing BarData with computed indicators organized as
 *           date -> coin -> BarData
 **************************************************************************************/
EnrichedData buildEnriched(const OHLCVData& raw)
{
    EnrichedData enriched;

    for (const auto& [coin, series] : raw.data)
    {
        std::vector<unsigned int> dates;
        dates.reserve(series.size());

        for (const auto& [ts, _] : series)
            dates.push_back(ts);

        std::vector<double> highs, lows, closes, opens, volumes;
        highs.reserve(series.size());
        lows.reserve(series.size());
        closes.reserve(series.size());
        opens.reserve(series.size());
        volumes.reserve(series.size());

        for (const auto& [ts, ohlcv] : series)
        {
            opens.push_back(ohlcv.open);
            highs.push_back(ohlcv.high);
            lows.push_back(ohlcv.low);
            closes.push_back(ohlcv.close);
            volumes.push_back(ohlcv.volume);
        }

        std::vector<double> tr(highs.size(), 0.0);

        // ================= TRUE RANGE =================
        for (size_t i = 1; i < highs.size(); ++i)
        {
            double hl = highs[i] - lows[i];
            double hc = std::abs(highs[i] - closes[i - 1]);
            double lc = std::abs(lows[i]  - closes[i - 1]);

            tr[i] = std::max({hl, hc, lc});
        }

        // ================= BUILD BAR DATA =================
        for (size_t i = 0; i < dates.size(); ++i)
        {
            BarData bar;

            bar.open   = opens[i];
            bar.high   = highs[i];
            bar.low    = lows[i];
            bar.close  = closes[i];
            bar.volume = volumes[i];
            bar.barNumber = static_cast<unsigned int>(i);

            // -------- 20D HIGH (previous 20 bars) --------
            if (i >= 20)
            {
                double maxHigh = highs[i - 20];

                for (size_t j = i - 20; j < i; ++j)
                    maxHigh = std::max(maxHigh, highs[j]);

                bar.high_20d = maxHigh;
            }
            else
            {
                bar.high_20d = 0.0;
            }

            // -------- ATR(14) using previous 14 TR --------
            if (i >= 14)
            {
                double sumTR = 0.0;

                for (size_t j = i - 14; j < i; ++j)
                    sumTR += tr[j];

                bar.atr_14d = sumTR / 14.0;
            }
            else
            {
                bar.atr_14d = 0.0;
            }

            enriched[dates[i]][coin] = bar;
        }
    }

    return enriched;
}