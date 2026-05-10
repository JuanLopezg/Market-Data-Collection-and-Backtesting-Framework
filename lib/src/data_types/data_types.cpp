#include "data_types.h"

#include <iostream>
#include <algorithm>
#include <cmath>

/**************************************************************************************
 * Purpose : Build enriched bar data with indicators from raw OHLCV market data
 *
 * This function processes OHLCV time series for each asset and computes additional
 * derived fields used in backtesting. For every completed bar it calculates:
 *
 *   - 20-day rolling high including the current bar
 *   - ATR(14) as simple mean of the last 14 true ranges including current TR
 *   - 25-day mean volume including the current bar
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
        // tr[0] remains 0 because there is no previous close for the first bar.
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
            bar.barNumber = static_cast<unsigned int>(i+1);

            // -------- 25D VOLUME MEAN including current bar --------
            // Equivalent to MA(V, 25)[0] once the current candle is closed.
            if (i >= 24)
            {
                double sumVolume = 0.0;

                for (size_t j = i - 24; j <= i; ++j)
                    sumVolume += volumes[j];

                bar.u25d_volume = sumVolume / 25.0;
            }
            else
            {
                bar.u25d_volume = 0.0;
            }

            // -------- MA50 CLOSE including current bar --------
            // Equivalent to MA(C, 50)[0] once the current candle is closed.
            if (i >= 49)
            {
                double sumClose = 0.0;

                for (size_t j = i - 49; j <= i; ++j)
                    sumClose += closes[j];

                bar.ma50 = sumClose / 50.0;
            }
            else
            {
                bar.ma50 = 0.0;
            }
            
            // -------- 20D HIGH including current bar --------
            // Equivalent to Highest(H, 20)[0] once the current candle is closed.
            if (i >= 19)
            {
                double maxHigh = highs[i - 19];

                for (size_t j = i - 19; j <= i; ++j)
                    maxHigh = std::max(maxHigh, highs[j]);

                bar.high_20d = maxHigh;
            }
            else
            {
                bar.high_20d = 0.0;
            }

            // -------- ATR(14) including current TR --------
            // This is a simple moving average of TR, equivalent to SMA(TR, 14)[0].
            // First valid value is at i == 14, using tr[1] ... tr[14].
            if (i >= 14)
            {
                double sumTR = 0.0;

                for (size_t j = i - 13; j <= i; ++j)
                    sumTR += tr[j];

                bar.atr_14d = sumTR / 14.0;
            }
            else
            {
                bar.atr_14d = 0.0;
            }

            // -------- ROC1 CLOSE --------
            // 1-bar rate of change based on close-to-close return.
            // Example: 0.05 = +5%, -0.03 = -3%.
            if (i >= 1 && closes[i - 1] != 0.0)
            {
                bar.roc1 = (closes[i] / closes[i - 1]) - 1.0;
            }
            else
            {
                bar.roc1 = 0.0;
            }

            enriched[dates[i]][coin] = bar;
        }
    }

    // ================= VOLUME RANK BY TIMESTAMP =================
    // For each timestamp, rank all coins by u25d_volume descending.
    // Rank 1 = highest 25D average volume at that timestamp.
    // Bars with u25d_volume == 0 are left as volumeRank = 0 because
    // they do not yet have a valid 25D volume mean.
    for (auto& [ts, barsByCoin] : enriched)
    {
        std::vector<decltype(barsByCoin.begin())> ranked;
        ranked.reserve(barsByCoin.size());

        for (auto it = barsByCoin.begin(); it != barsByCoin.end(); ++it)
        {
            it->second.volumeRank = 0;

            if (it->second.u25d_volume > 0.0)
                ranked.push_back(it);
        }

        std::sort(
            ranked.begin(),
            ranked.end(),
            [](const auto& a, const auto& b)
            {
                if (a->second.u25d_volume == b->second.u25d_volume)
                    return a->first < b->first; // deterministic tie-break

                return a->second.u25d_volume > b->second.u25d_volume;
            }
        );

        for (size_t i = 0; i < ranked.size(); ++i)
        {
            ranked[i]->second.volumeRank = static_cast<unsigned int>(i + 1);
        }
    }

    return enriched;
}