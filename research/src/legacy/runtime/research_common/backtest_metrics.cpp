#include "backtest_metrics.h"

#include "logger.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <random>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

struct DatedEquityPoint {
    Timestamp timestamp = 0;
    Balance balance = 0.0;
    Equity equity = 0.0;
};

bool splitTimestamp(Timestamp timestamp, int& year, unsigned int& month, unsigned int& day)
{
    year = static_cast<int>(timestamp / 10000U);
    month = (timestamp / 100U) % 100U;
    day = timestamp % 100U;

    return year > 0 && month >= 1U && month <= 12U && day >= 1U && day <= 31U;
}

// Days since an arbitrary civil-date epoch. Only date differences are used.
long long daysFromCivil(int year, unsigned int month, unsigned int day)
{
    year -= month <= 2U ? 1 : 0;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned int yearOfEra = static_cast<unsigned int>(year - era * 400);
    const unsigned int monthPrime = month > 2U ? month - 3U : month + 9U;
    const unsigned int dayOfYear = (153U * monthPrime + 2U) / 5U + day - 1U;
    const unsigned int dayOfEra =
        yearOfEra * 365U + yearOfEra / 4U - yearOfEra / 100U + dayOfYear;

    return static_cast<long long>(era) * 146097LL + static_cast<long long>(dayOfEra);
}

bool timestampToDayNumber(Timestamp timestamp, long long& dayNumber)
{
    int year = 0;
    unsigned int month = 0;
    unsigned int day = 0;

    if (!splitTimestamp(timestamp, year, month, day)) {
        return false;
    }

    dayNumber = daysFromCivil(year, month, day);
    return true;
}

std::vector<DatedEquityPoint> alignEquityCurve(
    const std::vector<std::pair<Balance, Equity>>& balanceEquityHistoric,
    const MarketData& marketData
)
{
    const std::size_t pointCount = std::min(balanceEquityHistoric.size(), marketData.size());

    std::vector<DatedEquityPoint> points;
    points.reserve(pointCount);

    auto marketIt = marketData.begin();
    for (std::size_t index = 0; index < pointCount; ++index, ++marketIt) {
        points.push_back(DatedEquityPoint{
            marketIt->first,
            balanceEquityHistoric[index].first,
            balanceEquityHistoric[index].second
        });
    }

    return points;
}

std::size_t countAnnualizationReferenceBars(
    const MarketData& marketData,
    const std::string& referenceSymbol
)
{
    if (marketData.empty()) {
        return 0U;
    }

    if (referenceSymbol.empty()) {
        return marketData.size();
    }

    std::size_t count = 0U;

    for (const auto& [timestamp, coinBars] : marketData) {
        (void)timestamp;

        if (coinBars.find(referenceSymbol) != coinBars.end()) {
            ++count;
        }
    }

    if (count == 0U) {
        LG_WARN(
            "Annualization benchmark symbol '{}' was not found in market data. "
            "Falling back to full market timeline length: {} bars.",
            referenceSymbol,
            marketData.size()
        );
        return marketData.size();
    }

    return count;
}

double barsPerCalendarDay(const BacktestMetricsSettings& settings)
{
    if (settings.periodsPerYear <= 0.0 || !std::isfinite(settings.periodsPerYear)) {
        return 1.0;
    }

    return std::max(1e-12, settings.periodsPerYear / 365.0);
}

double sampleStandardDeviation(const std::vector<double>& values, double mean)
{
    if (values.size() < 2U) {
        return 0.0;
    }

    double squaredDistanceSum = 0.0;
    for (const double value : values) {
        const double distance = value - mean;
        squaredDistanceSum += distance * distance;
    }

    return std::sqrt(squaredDistanceSum / static_cast<double>(values.size() - 1U));
}

double averageEquity(
    const std::vector<DatedEquityPoint>& points,
    double initialEquity
)
{
    double total = initialEquity;

    for (const DatedEquityPoint& point : points) {
        total += point.equity;
    }

    return total / static_cast<double>(points.size() + 1U);
}

bool shouldIncludeTrade(const Trade& trade, const BacktestMetricsSettings& settings)
{
    if (!trade.exited_) {
        return false;
    }

    return !settings.excludeSimulatedTrades || !trade.isSimulated_;
}

void calculateHistoricalTailRisk(
    const std::vector<double>& periodicReturns,
    BacktestMetrics& metrics
)
{
    if (periodicReturns.empty()) {
        return;
    }

    std::vector<double> sortedReturns = periodicReturns;
    std::sort(sortedReturns.begin(), sortedReturns.end());

    // Historical 95% VaR uses the lower (5%) empirical return quantile.
    constexpr double leftTailProbability = 0.05;
    const std::size_t quantileIndex = static_cast<std::size_t>(
        std::floor(leftTailProbability * static_cast<double>(sortedReturns.size() - 1U))
    );

    const double varThresholdReturn = sortedReturns[quantileIndex];
    metrics.historicalVaR95Percent = std::max(0.0, -varThresholdReturn * 100.0);

    double tailReturnSum = 0.0;
    std::size_t tailCount = 0U;

    for (const double periodReturn : sortedReturns) {
        if (periodReturn <= varThresholdReturn) {
            tailReturnSum += periodReturn;
            ++tailCount;
        }
    }

    if (tailCount > 0U) {
        const double averageTailReturn = tailReturnSum / static_cast<double>(tailCount);
        metrics.historicalCvar95Percent = std::max(0.0, -averageTailReturn * 100.0);
    }
}

void calculateDrawdownMetrics(
    const std::vector<DatedEquityPoint>& points,
    BacktestMetrics& metrics,
    const BacktestMetricsSettings& settings
)
{
    double peakEquity = metrics.initialEquity;
    std::size_t underwaterBars = 0U;
    bool isUnderwater = false;
    std::size_t underwaterStartIndex = 0U;
    const double barsPerDay = barsPerCalendarDay(settings);

    for (std::size_t index = 0; index < points.size(); ++index) {
        const DatedEquityPoint& point = points[index];

        if (!std::isfinite(point.equity) || peakEquity <= 0.0) {
            continue;
        }

        if (point.equity >= peakEquity) {
            peakEquity = point.equity;
            isUnderwater = false;
            continue;
        }

        const double drawdownAmount = point.equity - peakEquity;
        const double drawdownPercent = (-drawdownAmount / peakEquity) * 100.0;

        if (drawdownPercent > metrics.maxDrawdownPercent) {
            metrics.maxDrawdownPercent = drawdownPercent;
            metrics.maxDrawdownAmount = -drawdownAmount;
        }

        ++underwaterBars;

        if (!isUnderwater) {
            isUnderwater = true;
            underwaterStartIndex = index;
        }

        const std::size_t durationBars = index - underwaterStartIndex + 1U;
        metrics.maxDrawdownDurationBars = std::max(
            metrics.maxDrawdownDurationBars,
            durationBars
        );

        metrics.maxDrawdownDurationDays = std::max(
            metrics.maxDrawdownDurationDays,
            static_cast<double>(durationBars) / barsPerDay
        );
    }

    if (!points.empty()) {
        metrics.timeUnderwaterPercent =
            (static_cast<double>(underwaterBars) / static_cast<double>(points.size())) * 100.0;
    }
}

double netTradePnl(
    const Trade& trade,
    const BacktestMetricsSettings& settings
)
{
    if (!std::isfinite(trade.pnl_)) {
        return 0.0;
    }

    if (settings.tradePnlAlreadyNetOfCommission) {
        return trade.pnl_;
    }

    const double commission = std::isfinite(trade.commission_)
        ? std::max(0.0, trade.commission_)
        : 0.0;

    return trade.pnl_ - commission;
}

double percentile(
    std::vector<double> values,
    double percentileValue
)
{
    if (values.empty()) {
        return 0.0;
    }

    std::sort(values.begin(), values.end());

    const double boundedPercentile = std::clamp(percentileValue, 0.0, 100.0);
    const double position =
        (boundedPercentile / 100.0) * static_cast<double>(values.size() - 1U);

    const std::size_t lowerIndex =
        static_cast<std::size_t>(std::floor(position));
    const std::size_t upperIndex =
        static_cast<std::size_t>(std::ceil(position));

    if (lowerIndex == upperIndex) {
        return values[lowerIndex];
    }

    const double weight = position - static_cast<double>(lowerIndex);
    return values[lowerIndex] * (1.0 - weight) + values[upperIndex] * weight;
}

double probabilityOfAtLeast(
    const std::vector<double>& values,
    double threshold
)
{
    if (values.empty()) {
        return 0.0;
    }

    const std::size_t count = static_cast<std::size_t>(
        std::count_if(
            values.begin(),
            values.end(),
            [threshold](double value) {
                return value >= threshold;
            }
        )
    );

    return (static_cast<double>(count) / static_cast<double>(values.size())) * 100.0;
}

void calculateMonteCarloBootstrap(
    const std::vector<const Trade*>& closedTrades,
    double initialEquity,
    const BacktestMetricsSettings& settings,
    BacktestMetrics& metrics
)
{
    metrics.monteCarloMaxDrawdownPercentSamples.clear();
    metrics.monteCarloHistoricalTradeReturnPercentPath.clear();
    metrics.monteCarloFanReturnPercent.clear();

    if (
        settings.monteCarloSimulationCount == 0U ||
        initialEquity <= 0.0 ||
        !std::isfinite(initialEquity) ||
        closedTrades.empty()
    ) {
        return;
    }

    // Version 1 model: each historical closed-trade net PnL is normalized by
    // starting capital. Every simulated path samples that distribution with
    // replacement and applies the same number of trades as the historical run.
    std::vector<double> normalizedTradePnl;
    normalizedTradePnl.reserve(closedTrades.size());

    metrics.monteCarloHistoricalTradeReturnPercentPath.reserve(
        closedTrades.size() + 1U
    );
    metrics.monteCarloHistoricalTradeReturnPercentPath.push_back(0.0);

    double historicalEquity = initialEquity;

    for (const Trade* trade : closedTrades) {
        const double pnl = netTradePnl(*trade, settings);
        const double normalizedPnl = pnl / initialEquity;

        if (std::isfinite(normalizedPnl)) {
            normalizedTradePnl.push_back(normalizedPnl);
        }

        if (std::isfinite(pnl)) {
            historicalEquity += pnl;
            historicalEquity = std::max(0.0, historicalEquity);
        }

        metrics.monteCarloHistoricalTradeReturnPercentPath.push_back(
            ((historicalEquity / initialEquity) - 1.0) * 100.0
        );
    }

    if (normalizedTradePnl.empty()) {
        return;
    }

    metrics.monteCarloTradesPerSimulation = normalizedTradePnl.size();
    metrics.monteCarloMaxDrawdownPercentSamples.reserve(
        settings.monteCarloSimulationCount
    );

    const std::size_t requestedFanPointCount = std::max(
        std::size_t{2U},
        settings.monteCarloFanPointCount
    );
    const std::size_t fanPointCount = std::min(
        requestedFanPointCount,
        normalizedTradePnl.size() + 1U
    );

    std::vector<std::size_t> fanTradeIndexes;
    fanTradeIndexes.reserve(fanPointCount);

    for (std::size_t pointIndex = 0U; pointIndex < fanPointCount; ++pointIndex) {
        const double ratio = fanPointCount <= 1U
            ? 0.0
            : static_cast<double>(pointIndex) /
                  static_cast<double>(fanPointCount - 1U);

        fanTradeIndexes.push_back(
            static_cast<std::size_t>(std::llround(
                ratio * static_cast<double>(normalizedTradePnl.size())
            ))
        );
    }

    // Guard against floating-point rounding causing an out-of-range endpoint.
    fanTradeIndexes.front() = 0U;
    fanTradeIndexes.back() = normalizedTradePnl.size();

    std::vector<std::vector<double>> fanReturnSamples(fanPointCount);
    for (std::vector<double>& values : fanReturnSamples) {
        values.reserve(settings.monteCarloSimulationCount);
    }

    std::vector<double> maximumConsecutiveLossSamples;
    maximumConsecutiveLossSamples.reserve(settings.monteCarloSimulationCount);

    std::mt19937_64 randomEngine(settings.monteCarloSeed);
    std::uniform_int_distribution<std::size_t> sampleIndex(
        0U,
        normalizedTradePnl.size() - 1U
    );

    for (std::size_t simulation = 0U;
         simulation < settings.monteCarloSimulationCount;
         ++simulation) {
        double equity = initialEquity;
        double peakEquity = initialEquity;
        double maximumDrawdown = 0.0;

        std::size_t currentConsecutiveLosses = 0U;
        std::size_t maximumConsecutiveLosses = 0U;
        std::size_t nextFanPointIndex = 0U;
        bool validPath = true;

        auto appendFanSamplesThrough = [&](std::size_t completedTradeCount) {
            while (
                nextFanPointIndex < fanTradeIndexes.size() &&
                fanTradeIndexes[nextFanPointIndex] <= completedTradeCount
            ) {
                fanReturnSamples[nextFanPointIndex].push_back(
                    ((equity / initialEquity) - 1.0) * 100.0
                );
                ++nextFanPointIndex;
            }
        };

        appendFanSamplesThrough(0U);

        for (std::size_t tradeIndex = 0U;
             tradeIndex < normalizedTradePnl.size();
             ++tradeIndex) {
            const double sampledTradePnl =
                initialEquity * normalizedTradePnl[sampleIndex(randomEngine)];

            if (sampledTradePnl < 0.0) {
                ++currentConsecutiveLosses;
                maximumConsecutiveLosses = std::max(
                    maximumConsecutiveLosses,
                    currentConsecutiveLosses
                );
            } else {
                currentConsecutiveLosses = 0U;
            }

            equity += sampledTradePnl;

            if (!std::isfinite(equity)) {
                validPath = false;
                break;
            }

            const std::size_t completedTradeCount = tradeIndex + 1U;

            // Non-leveraged assumption: once equity reaches zero, the path is
            // ruined. Keep it at zero for all remaining fan-chart points.
            if (equity <= 0.0) {
                equity = 0.0;
                maximumDrawdown = 100.0;
                appendFanSamplesThrough(completedTradeCount);
                break;
            }

            peakEquity = std::max(peakEquity, equity);

            const double drawdown = std::clamp(
                ((peakEquity - equity) / peakEquity) * 100.0,
                0.0,
                100.0
            );
            maximumDrawdown = std::max(maximumDrawdown, drawdown);

            appendFanSamplesThrough(completedTradeCount);
        }

        if (!validPath) {
            continue;
        }

        // Fill any remaining fan points. This is required for a ruined path
        // or if no exact fan index matched due to a degenerate trade count.
        appendFanSamplesThrough(normalizedTradePnl.size());

        metrics.monteCarloMaxDrawdownPercentSamples.push_back(
            std::clamp(maximumDrawdown, 0.0, 100.0)
        );
        maximumConsecutiveLossSamples.push_back(
            static_cast<double>(maximumConsecutiveLosses)
        );
    }

    metrics.monteCarloSimulationCount =
        metrics.monteCarloMaxDrawdownPercentSamples.size();

    if (metrics.monteCarloSimulationCount == 0U) {
        return;
    }

    metrics.monteCarloProbabilityDrawdown20Percent =
        probabilityOfAtLeast(metrics.monteCarloMaxDrawdownPercentSamples, 20.0);
    metrics.monteCarloProbabilityDrawdown30Percent =
        probabilityOfAtLeast(metrics.monteCarloMaxDrawdownPercentSamples, 30.0);
    metrics.monteCarloProbabilityDrawdown50Percent =
        probabilityOfAtLeast(metrics.monteCarloMaxDrawdownPercentSamples, 50.0);

    // Explicit definition used in the report.
    metrics.monteCarloRiskOfRuinPercent =
        metrics.monteCarloProbabilityDrawdown50Percent;

    metrics.monteCarloMedianMaxDrawdownPercent =
        percentile(metrics.monteCarloMaxDrawdownPercentSamples, 50.0);
    metrics.monteCarloP95MaxDrawdownPercent =
        percentile(metrics.monteCarloMaxDrawdownPercentSamples, 95.0);
    metrics.monteCarloP99MaxDrawdownPercent =
        percentile(metrics.monteCarloMaxDrawdownPercentSamples, 99.0);

    metrics.monteCarloMedianMaxConsecutiveLosses =
        percentile(maximumConsecutiveLossSamples, 50.0);
    metrics.monteCarloP95MaxConsecutiveLosses =
        percentile(maximumConsecutiveLossSamples, 95.0);
    metrics.monteCarloP99MaxConsecutiveLosses =
        percentile(maximumConsecutiveLossSamples, 99.0);

    // Every successful simulation contributes one value at every fan point.
    // Convert the stored per-progress samples into percentile bands.
    metrics.monteCarloFanReturnPercent.reserve(fanPointCount);

    for (std::size_t pointIndex = 0U;
         pointIndex < fanPointCount;
         ++pointIndex) {
        const std::vector<double>& values = fanReturnSamples[pointIndex];

        if (values.size() != metrics.monteCarloSimulationCount) {
            // A non-finite simulation path was rejected. Keep the report safe
            // and omit this fan chart instead of mixing sample counts.
            metrics.monteCarloFanReturnPercent.clear();
            break;
        }

        const double progressPercent =
            normalizedTradePnl.empty()
                ? 0.0
                : (static_cast<double>(fanTradeIndexes[pointIndex]) /
                   static_cast<double>(normalizedTradePnl.size())) * 100.0;

        metrics.monteCarloFanReturnPercent.push_back(MonteCarloFanPoint{
            progressPercent,
            percentile(values, 5.0),
            percentile(values, 25.0),
            percentile(values, 50.0),
            percentile(values, 75.0),
            percentile(values, 95.0)
        });
    }
}

} // namespace

BacktestMetrics calculateBacktestMetrics(
    const std::string& strategyName,
    const std::map<TradeID, Trade>& tradesHistory,
    const std::vector<std::pair<Balance, Equity>>& balanceEquityHistoric,
    const MarketData& marketData,
    double initialEquity,
    const BacktestMetricsSettings& settings
)
{
    BacktestMetrics metrics;
    metrics.strategyName = strategyName;
    metrics.initialEquity = initialEquity;
    metrics.finalEquity = initialEquity;

    const std::vector<DatedEquityPoint> points =
        alignEquityCurve(balanceEquityHistoric, marketData);

    if (!points.empty()) {
        metrics.finalEquity = points.back().equity;
    }

    metrics.netProfit = metrics.finalEquity - metrics.initialEquity;

    if (metrics.initialEquity > 0.0 && metrics.finalEquity > 0.0) {
        metrics.netReturnPercent =
            ((metrics.finalEquity / metrics.initialEquity) - 1.0) * 100.0;
    }

    std::vector<double> periodicReturns;
    periodicReturns.reserve(points.size());

    double previousEquity = metrics.initialEquity;
    for (const DatedEquityPoint& point : points) {
        if (previousEquity > 0.0 && point.equity > 0.0) {
            periodicReturns.push_back((point.equity / previousEquity) - 1.0);
        }
        previousEquity = point.equity;
    }

    if (!periodicReturns.empty()) {
        const double meanReturn =
            std::accumulate(periodicReturns.begin(), periodicReturns.end(), 0.0) /
            static_cast<double>(periodicReturns.size());

        const double returnStandardDeviation =
            sampleStandardDeviation(periodicReturns, meanReturn);

        if (returnStandardDeviation > 0.0) {
            metrics.sharpeRatio =
                (meanReturn / returnStandardDeviation) * std::sqrt(settings.periodsPerYear);
        }

        double downsideSquaredSum = 0.0;
        for (const double periodReturn : periodicReturns) {
            const double downsideReturn = std::min(periodReturn, 0.0);
            downsideSquaredSum += downsideReturn * downsideReturn;
        }

        const double downsideDeviation = std::sqrt(
            downsideSquaredSum / static_cast<double>(periodicReturns.size())
        );

        if (downsideDeviation > 0.0) {
            metrics.sortinoRatio =
                (meanReturn / downsideDeviation) * std::sqrt(settings.periodsPerYear);
        }

        calculateHistoricalTailRisk(periodicReturns, metrics);
    }

    calculateDrawdownMetrics(points, metrics, settings);

    if (
        metrics.initialEquity > 0.0 &&
        metrics.finalEquity > 0.0 &&
        settings.periodsPerYear > 0.0
    ) {
        const std::size_t referenceBarCount =
            countAnnualizationReferenceBars(
                marketData,
                settings.annualizationBenchmarkSymbol
            );

        if (referenceBarCount > 0U) {
            const double years =
                static_cast<double>(referenceBarCount) / settings.periodsPerYear;

            if (years > 0.0 && std::isfinite(years)) {
                metrics.annualizedReturnPercent =
                    (std::pow(
                        metrics.finalEquity / metrics.initialEquity,
                        1.0 / years
                    ) - 1.0) * 100.0;
            }
        }
    }

    if (metrics.maxDrawdownPercent > 0.0) {
        metrics.calmarRatio =
            metrics.annualizedReturnPercent / metrics.maxDrawdownPercent;
    }

    std::vector<const Trade*> closedTrades;
    closedTrades.reserve(tradesHistory.size());

    for (const auto& [tradeId, trade] : tradesHistory) {
        (void)tradeId;

        if (shouldIncludeTrade(trade, settings)) {
            closedTrades.push_back(&trade);
        }
    }

    std::sort(
        closedTrades.begin(),
        closedTrades.end(),
        [](const Trade* left, const Trade* right) {
            return std::tie(left->end_, left->trade_id_) <
                   std::tie(right->end_, right->trade_id_);
        }
    );

    metrics.tradeCount = closedTrades.size();

    double totalPnl = 0.0;
    double totalHoldingBars = 0.0;
    double currentConsecutiveLossPnl = 0.0;
    std::size_t currentConsecutiveLosses = 0U;

    metrics.historicalTradeReturnPercentSamples.clear();
    metrics.historicalTradeReturnPercentSamples.reserve(closedTrades.size());

    for (const Trade* trade : closedTrades) {
        const double pnl = netTradePnl(*trade, settings);
        totalPnl += pnl;

        const double entryNotional = std::abs(trade->entry_ * trade->size_);
        if (std::isfinite(entryNotional) && entryNotional > 0.0) {
            const double tradeReturnPercent = (pnl / entryNotional) * 100.0;
            if (std::isfinite(tradeReturnPercent)) {
                metrics.historicalTradeReturnPercentSamples.push_back(
                    tradeReturnPercent
                );
            }
        }
        totalHoldingBars += static_cast<double>(trade->barsHeld);
        metrics.grossTurnover +=
            std::abs(trade->size_) * (std::abs(trade->entry_) + std::abs(trade->exit_));

        if (pnl > 0.0) {
            ++metrics.winningTradeCount;
            metrics.grossProfit += pnl;
            currentConsecutiveLosses = 0U;
            currentConsecutiveLossPnl = 0.0;
        } else if (pnl < 0.0) {
            ++metrics.losingTradeCount;
            metrics.grossLoss += -pnl;
            metrics.largestLoss = std::min(metrics.largestLoss, pnl);

            ++currentConsecutiveLosses;
            currentConsecutiveLossPnl += pnl;
            metrics.maximumConsecutiveLosses = std::max(
                metrics.maximumConsecutiveLosses,
                currentConsecutiveLosses
            );
            metrics.worstConsecutiveLossPnl = std::min(
                metrics.worstConsecutiveLossPnl,
                currentConsecutiveLossPnl
            );
        } else {
            ++metrics.breakevenTradeCount;
            currentConsecutiveLosses = 0U;
            currentConsecutiveLossPnl = 0.0;
        }
    }

    if (metrics.tradeCount > 0U) {
        metrics.expectancyPerTrade = totalPnl / static_cast<double>(metrics.tradeCount);
        metrics.winRatePercent =
            (static_cast<double>(metrics.winningTradeCount) /
             static_cast<double>(metrics.tradeCount)) * 100.0;
        metrics.averageHoldingBars =
            totalHoldingBars / static_cast<double>(metrics.tradeCount);
    }

    if (metrics.winningTradeCount > 0U) {
        metrics.averageWin =
            metrics.grossProfit / static_cast<double>(metrics.winningTradeCount);
    }

    if (metrics.losingTradeCount > 0U) {
        metrics.averageLoss =
            -metrics.grossLoss / static_cast<double>(metrics.losingTradeCount);
    }

    if (metrics.grossLoss > 0.0) {
        metrics.profitFactor = metrics.grossProfit / metrics.grossLoss;
    } else if (metrics.grossProfit > 0.0) {
        metrics.profitFactor = std::numeric_limits<double>::infinity();
    }

    if (!metrics.historicalTradeReturnPercentSamples.empty()) {
        metrics.historicalMeanTradeReturnPercent =
            std::accumulate(
                metrics.historicalTradeReturnPercentSamples.begin(),
                metrics.historicalTradeReturnPercentSamples.end(),
                0.0
            ) / static_cast<double>(metrics.historicalTradeReturnPercentSamples.size());
        metrics.historicalMedianTradeReturnPercent =
            percentile(metrics.historicalTradeReturnPercentSamples, 50.0);
        metrics.historicalP05TradeReturnPercent =
            percentile(metrics.historicalTradeReturnPercentSamples, 5.0);
        metrics.historicalP95TradeReturnPercent =
            percentile(metrics.historicalTradeReturnPercentSamples, 95.0);
    }

    const double meanEquity = averageEquity(points, metrics.initialEquity);
    if (meanEquity > 0.0) {
        metrics.turnoverMultiple = metrics.grossTurnover / meanEquity;
    }

    if (!marketData.empty() && !closedTrades.empty()) {
        std::vector<Timestamp> timestamps;
        timestamps.reserve(marketData.size());

        for (const auto& [timestamp, coinBars] : marketData) {
            (void)coinBars;
            timestamps.push_back(timestamp);
        }

        std::vector<int> exposureChanges(timestamps.size() + 1U, 0);

        for (const Trade* trade : closedTrades) {
            if (trade->start_ == 0U || trade->end_ == 0U || trade->end_ < trade->start_) {
                continue;
            }

            const auto startIt = std::lower_bound(
                timestamps.begin(), timestamps.end(), trade->start_
            );
            const auto endIt = std::upper_bound(
                timestamps.begin(), timestamps.end(), trade->end_
            );

            if (startIt == timestamps.end() || startIt >= endIt) {
                continue;
            }

            const std::size_t startIndex =
                static_cast<std::size_t>(std::distance(timestamps.begin(), startIt));
            const std::size_t endIndex =
                static_cast<std::size_t>(std::distance(timestamps.begin(), endIt) - 1);

            ++exposureChanges[startIndex];
            --exposureChanges[endIndex + 1U];
        }

        int activeTradeCount = 0;
        std::size_t exposedBars = 0U;

        for (std::size_t index = 0; index < timestamps.size(); ++index) {
            activeTradeCount += exposureChanges[index];
            if (activeTradeCount > 0) {
                ++exposedBars;
            }
        }

        metrics.exposurePercent =
            (static_cast<double>(exposedBars) / static_cast<double>(timestamps.size())) * 100.0;
    }

    calculateMonteCarloBootstrap(
        closedTrades,
        metrics.initialEquity,
        settings,
        metrics
    );

    return metrics;
}

void logBacktestMetrics(const BacktestMetrics& metrics)
{
    LG_INFO("============================================================");
    LG_INFO("Metrics for strategy: {}", metrics.strategyName);
    LG_INFO(
        "Equity: initial={:.2f} final={:.2f} net_profit={:.2f} net_return={:.2f}% annualized_return={:.2f}%",
        metrics.initialEquity,
        metrics.finalEquity,
        metrics.netProfit,
        metrics.netReturnPercent,
        metrics.annualizedReturnPercent
    );
    LG_INFO(
        "Risk: max_drawdown={:.2f} ({:.2f}%) max_dd_duration={} bars ({:.0f} days) underwater={:.2f}% var95={:.2f}% cvar95={:.2f}% sharpe={:.3f} sortino={:.3f} calmar={:.3f}",
        metrics.maxDrawdownAmount,
        metrics.maxDrawdownPercent,
        metrics.maxDrawdownDurationBars,
        metrics.maxDrawdownDurationDays,
        metrics.timeUnderwaterPercent,
        metrics.historicalVaR95Percent,
        metrics.historicalCvar95Percent,
        metrics.sharpeRatio,
        metrics.sortinoRatio,
        metrics.calmarRatio
    );
    LG_INFO(
        "Trades: total={} wins={} losses={} breakeven={} win_rate={:.2f}% profit_factor={:.3f} expectancy={:.2f}",
        metrics.tradeCount,
        metrics.winningTradeCount,
        metrics.losingTradeCount,
        metrics.breakevenTradeCount,
        metrics.winRatePercent,
        metrics.profitFactor,
        metrics.expectancyPerTrade
    );
    LG_INFO(
        "Trade PnL: gross_profit={:.2f} gross_loss={:.2f} average_win={:.2f} average_loss={:.2f} largest_loss={:.2f}",
        metrics.grossProfit,
        metrics.grossLoss,
        metrics.averageWin,
        metrics.averageLoss,
        metrics.largestLoss
    );
    LG_INFO(
        "Historical trade return (% entry notional): mean={:.2f}% p05={:.2f}% median={:.2f}% p95={:.2f}% samples={}",
        metrics.historicalMeanTradeReturnPercent,
        metrics.historicalP05TradeReturnPercent,
        metrics.historicalMedianTradeReturnPercent,
        metrics.historicalP95TradeReturnPercent,
        metrics.historicalTradeReturnPercentSamples.size()
    );
    LG_INFO(
        "Portfolio activity: max_consecutive_losses={} worst_loss_streak={:.2f} avg_holding_bars={:.2f} exposure={:.2f}% gross_turnover={:.2f} turnover_multiple={:.2f}x",
        metrics.maximumConsecutiveLosses,
        metrics.worstConsecutiveLossPnl,
        metrics.averageHoldingBars,
        metrics.exposurePercent,
        metrics.grossTurnover,
        metrics.turnoverMultiple
    );

    if (metrics.monteCarloSimulationCount > 0U) {
        LG_INFO(
            "Monte Carlo: simulations={} trades_per_path={} p_dd20={:.2f}% p_dd30={:.2f}% p_dd50={:.2f}% risk_of_ruin={:.2f}%",
            metrics.monteCarloSimulationCount,
            metrics.monteCarloTradesPerSimulation,
            metrics.monteCarloProbabilityDrawdown20Percent,
            metrics.monteCarloProbabilityDrawdown30Percent,
            metrics.monteCarloProbabilityDrawdown50Percent,
            metrics.monteCarloRiskOfRuinPercent
        );
        LG_INFO(
            "Monte Carlo tails: median_dd={:.2f}% p95_dd={:.2f}% p99_dd={:.2f}% median_loss_streak={:.1f} p95_loss_streak={:.1f} p99_loss_streak={:.1f}",
            metrics.monteCarloMedianMaxDrawdownPercent,
            metrics.monteCarloP95MaxDrawdownPercent,
            metrics.monteCarloP99MaxDrawdownPercent,
            metrics.monteCarloMedianMaxConsecutiveLosses,
            metrics.monteCarloP95MaxConsecutiveLosses,
            metrics.monteCarloP99MaxConsecutiveLosses
        );
    }
}
