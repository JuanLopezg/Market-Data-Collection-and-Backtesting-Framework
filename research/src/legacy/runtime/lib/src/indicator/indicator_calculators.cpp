#include "indicator_calculators.h"

#include <algorithm>
#include <cmath>
#include <limits>


namespace {

double invalidValue()
{
    return std::numeric_limits<double>::quiet_NaN();
}


std::vector<double> invalidVector(std::size_t size)
{
    return std::vector<double>(size, invalidValue());
}


double getField(const OHLCV& bar, PriceField source)
{
    switch (source) {
        case PriceField::Open:
            return bar.open;
        case PriceField::High:
            return bar.high;
        case PriceField::Low:
            return bar.low;
        case PriceField::Close:
            return bar.close;
        case PriceField::Volume:
            return bar.volume;
        default:
            return invalidValue();
    }
}


std::vector<double> extractSeries(
    const std::vector<OHLCV>& bars,
    PriceField source
)
{
    std::vector<double> values;
    values.reserve(bars.size());

    for (const auto& bar : bars) {
        values.push_back(getField(bar, source));
    }

    return values;
}


double calculateRSIValue(double avgGain, double avgLoss)
{
    if (avgGain == 0.0 && avgLoss == 0.0) {
        return 50.0;
    }

    if (avgLoss == 0.0) {
        return 100.0;
    }

    if (avgGain == 0.0) {
        return 0.0;
    }

    const double rs = avgGain / avgLoss;
    return 100.0 - (100.0 / (1.0 + rs));
}

} // namespace


std::vector<double> calculateSMA(
    const std::vector<OHLCV>& bars,
    PriceField source,
    unsigned int length
)
{
    std::vector<double> result = invalidVector(bars.size());

    if (length == 0 || bars.empty()) {
        return result;
    }

    const auto values = extractSeries(bars, source);
    const std::size_t n = values.size();
    const std::size_t len = static_cast<std::size_t>(length);

    double sum = 0.0;

    for (std::size_t i = 0; i < n; ++i) {
        sum += values[i];

        if (i >= len) {
            sum -= values[i - len];
        }

        if (i + 1 >= len) {
            result[i] = sum / static_cast<double>(len);
        }
    }

    return result;
}


std::vector<double> calculateEMA(
    const std::vector<OHLCV>& bars,
    PriceField source,
    unsigned int length
)
{
    std::vector<double> result = invalidVector(bars.size());

    if (length == 0 || bars.empty()) {
        return result;
    }

    const auto values = extractSeries(bars, source);
    const std::size_t n = values.size();
    const std::size_t len = static_cast<std::size_t>(length);

    if (n < len) {
        return result;
    }

    double initialSum = 0.0;

    for (std::size_t i = 0; i < len; ++i) {
        initialSum += values[i];
    }

    const double alpha = 2.0 / (static_cast<double>(len) + 1.0);

    result[len - 1] = initialSum / static_cast<double>(len);

    for (std::size_t i = len; i < n; ++i) {
        result[i] = alpha * values[i] + (1.0 - alpha) * result[i - 1];
    }

    return result;
}


std::vector<double> calculateROC(
    const std::vector<OHLCV>& bars,
    PriceField source,
    unsigned int length
)
{
    std::vector<double> result = invalidVector(bars.size());

    if (length == 0 || bars.empty()) {
        return result;
    }

    const auto values = extractSeries(bars, source);
    const std::size_t n = values.size();
    const std::size_t len = static_cast<std::size_t>(length);

    for (std::size_t i = len; i < n; ++i) {
        const double previous = values[i - len];

        if (previous != 0.0) {
            result[i] = (values[i] / previous) - 1.0;
        }
    }

    return result;
}


std::vector<double> calculateRSI(
    const std::vector<OHLCV>& bars,
    PriceField source,
    unsigned int length
)
{
    std::vector<double> result = invalidVector(bars.size());

    if (length == 0 || bars.empty()) {
        return result;
    }

    const auto values = extractSeries(bars, source);
    const std::size_t n = values.size();
    const std::size_t len = static_cast<std::size_t>(length);

    if (n <= len) {
        return result;
    }

    double gainSum = 0.0;
    double lossSum = 0.0;

    for (std::size_t i = 1; i <= len; ++i) {
        const double change = values[i] - values[i - 1];

        if (change > 0.0) {
            gainSum += change;
        } else {
            lossSum += std::abs(change);
        }
    }

    double avgGain = gainSum / static_cast<double>(len);
    double avgLoss = lossSum / static_cast<double>(len);

    result[len] = calculateRSIValue(avgGain, avgLoss);

    for (std::size_t i = len + 1; i < n; ++i) {
        const double change = values[i] - values[i - 1];

        const double gain = change > 0.0 ? change : 0.0;
        const double loss = change < 0.0 ? std::abs(change) : 0.0;

        avgGain = ((avgGain * static_cast<double>(len - 1)) + gain) / static_cast<double>(len);
        avgLoss = ((avgLoss * static_cast<double>(len - 1)) + loss) / static_cast<double>(len);

        result[i] = calculateRSIValue(avgGain, avgLoss);
    }

    return result;
}


std::vector<double> calculateATR(
    const std::vector<OHLCV>& bars,
    unsigned int length
)
{
    std::vector<double> result = invalidVector(bars.size());

    if (length == 0 || bars.empty()) {
        return result;
    }

    const std::size_t n = bars.size();
    const std::size_t len = static_cast<std::size_t>(length);

    if (n <= len) {
        return result;
    }

    std::vector<double> trueRange = invalidVector(n);

    for (std::size_t i = 1; i < n; ++i) {
        const double highLow = bars[i].high - bars[i].low;
        const double highClose = std::abs(bars[i].high - bars[i - 1].close);
        const double lowClose = std::abs(bars[i].low - bars[i - 1].close);

        trueRange[i] = std::max({highLow, highClose, lowClose});
    }

    double sumTR = 0.0;

    for (std::size_t i = 1; i <= len; ++i) {
        sumTR += trueRange[i];
    }

    result[len] = sumTR / static_cast<double>(len);

    for (std::size_t i = len + 1; i < n; ++i) {
        result[i] =
            ((result[i - 1] * static_cast<double>(len - 1)) + trueRange[i])
            / static_cast<double>(len);
    }

    return result;
}


std::vector<double> calculateHighest(
    const std::vector<OHLCV>& bars,
    PriceField source,
    unsigned int length
)
{
    std::vector<double> result = invalidVector(bars.size());

    if (length == 0 || bars.empty()) {
        return result;
    }

    const auto values = extractSeries(bars, source);
    const std::size_t n = values.size();
    const std::size_t len = static_cast<std::size_t>(length);

    if (n < len) {
        return result;
    }

    for (std::size_t i = len - 1; i < n; ++i) {
        double highest = values[i + 1 - len];

        for (std::size_t j = i + 1 - len; j <= i; ++j) {
            highest = std::max(highest, values[j]);
        }

        result[i] = highest;
    }

    return result;
}


std::vector<double> calculateLowest(
    const std::vector<OHLCV>& bars,
    PriceField source,
    unsigned int length
)
{
    std::vector<double> result = invalidVector(bars.size());

    if (length == 0 || bars.empty()) {
        return result;
    }

    const auto values = extractSeries(bars, source);
    const std::size_t n = values.size();
    const std::size_t len = static_cast<std::size_t>(length);

    if (n < len) {
        return result;
    }

    for (std::size_t i = len - 1; i < n; ++i) {
        double lowest = values[i + 1 - len];

        for (std::size_t j = i + 1 - len; j <= i; ++j) {
            lowest = std::min(lowest, values[j]);
        }

        result[i] = lowest;
    }

    return result;
}


std::vector<double> calculateDonchianHigh(
    const std::vector<OHLCV>& bars,
    unsigned int length
)
{
    return calculateHighest(bars, PriceField::High, length);
}


std::vector<double> calculateDonchianLow(
    const std::vector<OHLCV>& bars,
    unsigned int length
)
{
    return calculateLowest(bars, PriceField::Low, length);
}


std::vector<double> calculateDonchianMid(
    const std::vector<OHLCV>& bars,
    unsigned int length
)
{
    std::vector<double> result = invalidVector(bars.size());

    const auto high = calculateDonchianHigh(bars, length);
    const auto low = calculateDonchianLow(bars, length);

    for (std::size_t i = 0; i < bars.size(); ++i) {
        if (std::isfinite(high[i]) && std::isfinite(low[i])) {
            result[i] = (high[i] + low[i]) / 2.0;
        }
    }

    return result;
}
