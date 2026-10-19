#include <gtest/gtest.h>

#include "indicator_calculators.h"
#include "indicator_engine.h"

#include <cmath>
#include <initializer_list>
#include <limits>
#include <vector>

namespace {

constexpr double unavailable = std::numeric_limits<double>::quiet_NaN();

std::vector<OHLCV> prices(std::initializer_list<double> closes)
{
    std::vector<OHLCV> bars;
    for (double close : closes) bars.push_back({close, close + 1, close - 1, close, 1});
    return bars;
}

std::vector<OHLCV> quoteBars(std::initializer_list<double> turnover)
{
    std::vector<OHLCV> bars;
    for (double quote : turnover) bars.push_back({100, 120, 90, 110, 3, quote});
    return bars;
}

OHLCVData history(const std::vector<OHLCV>& bars, const Coin& coin = "BTC")
{
    OHLCVData raw;
    for (std::size_t i = 0; i < bars.size(); ++i)
        raw.data[coin][20240101u + static_cast<Timestamp>(i)] = bars[i];
    return raw;
}

void expectValues(const std::vector<double>& actual, std::initializer_list<double> expected)
{
    ASSERT_EQ(actual.size(), expected.size());
    std::size_t i = 0;
    for (double value : expected) {
        SCOPED_TRACE(i);
        if (std::isnan(value)) EXPECT_TRUE(std::isnan(actual[i]));
        else EXPECT_NEAR(actual[i], value, 1e-10);
        ++i;
    }
}

std::vector<std::vector<double>> allCalculations(const std::vector<OHLCV>& bars, unsigned length)
{
    return {
        calculateSMA(bars, PriceField::Close, length),
        calculateEMA(bars, PriceField::Close, length),
        calculateRSI(bars, PriceField::Close, length),
        calculateROC(bars, PriceField::Close, length),
        calculateATR(bars, length),
        calculateHighest(bars, PriceField::Close, length),
        calculateLowest(bars, PriceField::Close, length),
        calculateDonchianHigh(bars, length),
        calculateDonchianLow(bars, length),
        calculateDonchianMid(bars, length)
    };
}

TEST(IndicatorCalculations, R1_EmptyInputCannotProduceFeatures)
{
    for (const auto& result : allCalculations({}, 3)) EXPECT_TRUE(result.empty());
}

TEST(IndicatorCalculations, R1_ZeroLengthCannotProduceUsableFeatures)
{
    for (const auto& result : allCalculations(prices({10, 20, 30}), 0))
        expectValues(result, {unavailable, unavailable, unavailable});
}

TEST(IndicatorCalculations, R1_InsufficientHistoryIsUnavailableRatherThanZero)
{
    for (const auto& result : allCalculations(prices({10, 20}), 3))
        expectValues(result, {unavailable, unavailable});
    expectValues(calculateRSI(prices({10, 20, 30}), PriceField::Close, 3),
                 {unavailable, unavailable, unavailable});
    expectValues(calculateROC(prices({10, 20, 30}), PriceField::Close, 3),
                 {unavailable, unavailable, unavailable});
}

TEST(IndicatorCalculations, R2_SmaUsesTheLatestCompleteEqualWeightWindow)
{
    expectValues(calculateSMA(prices({10, 20, 30, 40, 50}), PriceField::Close, 3),
                 {unavailable, unavailable, 20, 30, 40});
}

TEST(IndicatorCalculations, R2_ConfiguredSourcePreservesPriceAndVolumeUnits)
{
    const std::vector<OHLCV> bars{{10, 13, 8, 12, 5, 41}, {20, 25, 17, 22, 7, 63}};
    expectValues(calculateSMA(bars, PriceField::Open, 2), {unavailable, 15});
    expectValues(calculateSMA(bars, PriceField::High, 2), {unavailable, 19});
    expectValues(calculateSMA(bars, PriceField::Low, 2), {unavailable, 12.5});
    expectValues(calculateSMA(bars, PriceField::Close, 2), {unavailable, 17});
    expectValues(calculateSMA(bars, PriceField::Volume, 2), {unavailable, 6});
    expectValues(calculateSMA(bars, PriceField::QuoteVolume, 2), {unavailable, 52});
}

TEST(IndicatorCalculations, R2_QuoteSmaRecoversWhenTheMissingRowLeavesItsWindow)
{
    expectValues(calculateSMA(quoteBars({10, unavailable, 30, 50, 70}), PriceField::QuoteVolume, 2),
                 {unavailable, unavailable, unavailable, 40, 60});
}

TEST(IndicatorCalculations, R2_InvalidTurnoverIsUnavailableAndCannotPoisonLaterWindows)
{
    for (double invalid : {-1.0, std::numeric_limits<double>::infinity()})
        expectValues(calculateSMA(quoteBars({10, invalid, 30, 50}), PriceField::QuoteVolume, 2),
                     {unavailable, unavailable, unavailable, 40});
}

TEST(IndicatorCalculations, R2_ZeroTurnoverIsARealObservation)
{
    expectValues(calculateSMA(quoteBars({0, 20}), PriceField::QuoteVolume, 2), {unavailable, 10});
}

TEST(IndicatorCalculations, R3_EmaSeedsWithSmaThenUsesItsExponentialAlpha)
{
    expectValues(calculateEMA(prices({10, 20, 30, 50, 10}), PriceField::Close, 3),
                 {unavailable, unavailable, 20, 35, 22.5});
}

TEST(IndicatorCalculations, R3_LengthOneEmaTracksTheCurrentObservation)
{
    expectValues(calculateEMA(prices({10, 20, 30, 50, 10}), PriceField::Close, 1),
                 {10, 20, 30, 50, 10});
}

TEST(IndicatorCalculations, R4_RsiUsesWilderSeedAndRecursiveGainLossAverages)
{
    expectValues(calculateRSI(prices({10, 12, 11, 13, 12, 14}), PriceField::Close, 3),
                 {unavailable, unavailable, unavailable, 80, 800.0 / 13.0, 850.0 / 11.0});
}

TEST(IndicatorCalculations, R4_RsiHasDefinedAllGainAllLossAndFlatLimits)
{
    expectValues(calculateRSI(prices({10, 11, 12, 13, 14}), PriceField::Close, 3),
                 {unavailable, unavailable, unavailable, 100, 100});
    expectValues(calculateRSI(prices({14, 13, 12, 11, 10}), PriceField::Close, 3),
                 {unavailable, unavailable, unavailable, 0, 0});
    expectValues(calculateRSI(prices({10, 10, 10, 10, 10}), PriceField::Close, 3),
                 {unavailable, unavailable, unavailable, 50, 50});
}

TEST(IndicatorCalculations, R4_LengthOneRsiNeedsOneCompletedChange)
{
    expectValues(calculateRSI(prices({10, 12, 11, 13}), PriceField::Close, 1),
                 {unavailable, 100, 0, 100});
}

TEST(IndicatorCalculations, R4_RsiIsBoundedAndInvariantUnderPositiveScaleOrTranslation)
{
    const auto bars = prices({10, 12, 11, 13, 12, 14});
    auto scaled = bars;
    auto shifted = bars;
    for (auto& bar : scaled) {
        bar.open *= 10;
        bar.high *= 10;
        bar.low *= 10;
        bar.close *= 10;
    }
    for (auto& bar : shifted) {
        bar.open += 100;
        bar.high += 100;
        bar.low += 100;
        bar.close += 100;
    }
    const auto original = calculateRSI(bars, PriceField::Close, 3);
    const auto scaleResult = calculateRSI(scaled, PriceField::Close, 3);
    const auto shiftResult = calculateRSI(shifted, PriceField::Close, 3);
    ASSERT_EQ(original.size(), 6u);
    ASSERT_EQ(scaleResult.size(), original.size());
    ASSERT_EQ(shiftResult.size(), original.size());
    for (std::size_t i = 3; i < original.size(); ++i) {
        EXPECT_GE(original[i], 0);
        EXPECT_LE(original[i], 100);
        EXPECT_NEAR(scaleResult[i], original[i], 1e-10);
        EXPECT_NEAR(shiftResult[i], original[i], 1e-10);
    }
}

TEST(IndicatorCalculations, R5_RocReportsFractionalReturnsAtTheRequestedLag)
{
    expectValues(calculateROC(prices({100, 110, 90, 120}), PriceField::Close, 2),
                 {unavailable, unavailable, -0.1, 1.0 / 11.0});
}

TEST(IndicatorCalculations, R5_ZeroDenominatorIsUnavailableButZeroNumeratorIsValid)
{
    auto bars = prices({100, 100, 100});
    bars[0].volume = 0;
    bars[1].volume = 5;
    bars[2].volume = 0;
    expectValues(calculateROC(bars, PriceField::Volume, 1), {unavailable, unavailable, -1});
}

std::vector<OHLCV> trueRangeExample()
{
    return {{100, 102, 98, 100, 1}, {102, 103, 99, 102, 1},
            {104, 108, 102, 106, 1}, {110, 116, 108, 112, 1}};
}

TEST(IndicatorCalculations, R6_AtrSeedNeedsTwoTrueRangesAndTheirPreviousCloses)
{
    auto bars = trueRangeExample();
    bars.resize(3);
    expectValues(calculateATR(bars, 2), {unavailable, unavailable, 5});
}

TEST(IndicatorCalculations, R6_AtrUsesSimpleRollingTrueRangeIncludingGaps)
{
    expectValues(calculateATR(trueRangeExample(), 2), {unavailable, unavailable, 5, 8});
}

TEST(IndicatorCalculations, R7_ExtremaExpireWhenTheyLeaveTheObservationWindow)
{
    const auto bars = prices({10, 30, 20, 5});
    expectValues(calculateHighest(bars, PriceField::Close, 2), {unavailable, 30, 30, 20});
    expectValues(calculateLowest(bars, PriceField::Close, 2), {unavailable, 10, 20, 5});
}

TEST(IndicatorCalculations, R7_DonchianBandsUseHighLowAndTheirMidpoint)
{
    const std::vector<OHLCV> bars{{10, 12, 8, 10, 1}, {12, 15, 9, 12, 1},
                                  {10, 14, 7, 10, 1}, {15, 20, 11, 15, 1}};
    expectValues(calculateDonchianHigh(bars, 2), {unavailable, 15, 15, 20});
    expectValues(calculateDonchianLow(bars, 2), {unavailable, 8, 7, 7});
    expectValues(calculateDonchianMid(bars, 2), {unavailable, 11.5, 11, 13.5});
}

TEST(IndicatorCache, R8_FutureExtremeCannotChangeAnyEarlierFeatureOrOffset)
{
    auto prefix = history(prices({10, 12, 11, 13}));
    auto full = prefix;
    full.data["BTC"][20240105] = {1000000, 1000010, 999990, 1000000, 1};
    std::vector<IndicatorSpec> specs;
    for (IndicatorKind kind : {IndicatorKind::SMA, IndicatorKind::EMA, IndicatorKind::RSI,
            IndicatorKind::ATR, IndicatorKind::ROC, IndicatorKind::Highest, IndicatorKind::Lowest,
            IndicatorKind::DonchianHigh, IndicatorKind::DonchianLow, IndicatorKind::DonchianMid}) {
        specs.push_back({kind, PriceField::Close, 2});
        specs.push_back({kind, PriceField::Close, 2, 1});
    }
    IndicatorEngine prefixEngine;
    IndicatorEngine fullEngine;
    prefixEngine.precompute(prefix, specs);
    fullEngine.precompute(full, specs);
    for (const auto& spec : specs) {
        SCOPED_TRACE(indicatorSpecToString(spec));
        for (Timestamp date : {20240101u, 20240102u, 20240103u, 20240104u}) {
            SCOPED_TRACE(date);
            const auto original = prefixEngine.value("BTC", date, spec);
            const auto withFuture = fullEngine.value("BTC", date, spec);
            if (std::isnan(original)) EXPECT_TRUE(std::isnan(withFuture));
            else EXPECT_NEAR(withFuture, original, 1e-10);
        }
    }
    const IndicatorSpec sma{IndicatorKind::SMA, PriceField::Close, 2};
    EXPECT_DOUBLE_EQ(fullEngine.value("BTC", 20240102, sma), 11);
}

TEST(IndicatorCache, R8_OffsetReadsThePreviousObservedBarAcrossCalendarGaps)
{
    OHLCVData raw;
    raw.data["BTC"][20240101] = {10, 11, 9, 10, 1};
    raw.data["BTC"][20240103] = {20, 21, 19, 20, 1};
    raw.data["BTC"][20240104] = {30, 31, 29, 30, 1};
    const IndicatorSpec current{IndicatorKind::SMA, PriceField::Close, 2};
    const IndicatorSpec previous{IndicatorKind::SMA, PriceField::Close, 2, 1};
    IndicatorEngine engine;
    engine.precompute(raw, {current, previous});
    EXPECT_DOUBLE_EQ(engine.value("BTC", 20240104, current), 25);
    EXPECT_DOUBLE_EQ(engine.value("BTC", 20240104, previous), 15);
    EXPECT_FALSE(engine.has("BTC", 20240103, previous));
    EXPECT_FALSE(engine.has("BTC", 20240102, current));
}

TEST(IndicatorCache, R8_OffsetBeyondHistoryCannotProduceAFeature)
{
    const IndicatorSpec spec{IndicatorKind::SMA, PriceField::Close, 1, 3};
    IndicatorEngine engine;
    engine.precompute(history(prices({10, 20, 30})), {spec});
    for (Timestamp date : {20240101u, 20240102u, 20240103u}) {
        EXPECT_FALSE(engine.has("BTC", date, spec));
        EXPECT_TRUE(std::isnan(engine.value("BTC", date, spec)));
    }
}

TEST(IndicatorCache, R9_KindSourceLengthAndOffsetHaveIndependentValues)
{
    const IndicatorSpec sma3{IndicatorKind::SMA, PriceField::Close, 3};
    const IndicatorSpec sma2{IndicatorKind::SMA, PriceField::Close, 2};
    const IndicatorSpec ema3{IndicatorKind::EMA, PriceField::Close, 3};
    const IndicatorSpec volume{IndicatorKind::SMA, PriceField::Volume, 3};
    const IndicatorSpec previous{IndicatorKind::SMA, PriceField::Close, 3, 1};
    IndicatorEngine engine;
    engine.precompute(history(prices({10, 20, 30, 50, 10})), {sma3, sma2, ema3, volume, previous});
    EXPECT_NEAR(engine.value("BTC", 20240104, sma3), 100.0 / 3.0, 1e-10);
    EXPECT_DOUBLE_EQ(engine.value("BTC", 20240104, sma2), 40);
    EXPECT_DOUBLE_EQ(engine.value("BTC", 20240104, ema3), 35);
    EXPECT_DOUBLE_EQ(engine.value("BTC", 20240104, volume), 1);
    EXPECT_DOUBLE_EQ(engine.value("BTC", 20240104, previous), 20);
}

TEST(IndicatorCache, R9_MissingAssetDateSpecAndWarmupAreUnavailable)
{
    const IndicatorSpec sma{IndicatorKind::SMA, PriceField::Close, 2};
    const IndicatorSpec ema{IndicatorKind::EMA, PriceField::Close, 2};
    IndicatorEngine engine;
    engine.precompute(history(prices({10, 20, 30})), {sma});
    EXPECT_FALSE(engine.has("UNKNOWN", 20240103, sma));
    EXPECT_FALSE(engine.has("BTC", 20240104, sma));
    EXPECT_FALSE(engine.has("BTC", 20240103, ema));
    EXPECT_FALSE(engine.has("BTC", 20240101, sma));
    EXPECT_TRUE(std::isnan(engine.value("UNKNOWN", 20240103, sma)));
    EXPECT_TRUE(std::isnan(engine.value("BTC", 20240104, sma)));
    EXPECT_TRUE(std::isnan(engine.value("BTC", 20240103, ema)));
    EXPECT_TRUE(std::isnan(engine.value("BTC", 20240101, sma)));
}

TEST(IndicatorCache, R9_ValidZeroReturnRemainsAvailable)
{
    const IndicatorSpec roc{IndicatorKind::ROC, PriceField::Close, 1};
    IndicatorEngine engine;
    engine.precompute(history(prices({10, 10})), {roc});
    EXPECT_TRUE(engine.has("BTC", 20240102, roc));
    EXPECT_DOUBLE_EQ(engine.value("BTC", 20240102, roc), 0);
}

TEST(IndicatorCache, R9_RepeatedPrecomputeDuplicateSpecsAndReconstructionAgree)
{
    const auto raw = history(prices({10, 20, 30}));
    const IndicatorSpec sma{IndicatorKind::SMA, PriceField::Close, 2};
    IndicatorEngine engine;
    engine.precompute(raw, {sma, sma});
    EXPECT_DOUBLE_EQ(engine.value("BTC", 20240103, sma), 25);
    engine.precompute(raw, {sma});
    IndicatorEngine restarted;
    restarted.precompute(raw, {sma, sma});
    EXPECT_DOUBLE_EQ(engine.value("BTC", 20240103, sma), 25);
    EXPECT_DOUBLE_EQ(restarted.value("BTC", 20240103, sma), 25);
}

TEST(IndicatorCache, R9_RecomputeReplacesOldAssetsDatesAndSpecifications)
{
    const IndicatorSpec sma{IndicatorKind::SMA, PriceField::Close, 2};
    const IndicatorSpec ema{IndicatorKind::EMA, PriceField::Close, 2};
    IndicatorEngine engine;
    engine.precompute(history(prices({10, 20, 30})), {sma, ema});
    engine.precompute(history(prices({5, 5}), "ETH"), {sma});
    EXPECT_FALSE(engine.has("BTC", 20240103, sma));
    EXPECT_FALSE(engine.has("ETH", 20240103, sma));
    EXPECT_FALSE(engine.has("ETH", 20240102, ema));
    EXPECT_DOUBLE_EQ(engine.value("ETH", 20240102, sma), 5);
}

TEST(IndicatorCache, R9_CallerMutationCannotRewritePreviouslyComputedFeatures)
{
    auto raw = history(prices({10, 20, 30}));
    const IndicatorSpec sma{IndicatorKind::SMA, PriceField::Close, 2};
    IndicatorEngine engine;
    engine.precompute(raw, {sma});
    raw.data["BTC"][20240103].close = 1000000;
    raw.data.clear();
    EXPECT_DOUBLE_EQ(engine.value("BTC", 20240103, sma), 25);
}

TEST(IndicatorCache, R10_InfiniteFeatureCannotAdvertiseAvailability)
{
    auto raw = history(prices({10}));
    raw.data["BTC"][20240101].high = std::numeric_limits<double>::infinity();
    const IndicatorSpec highest{IndicatorKind::Highest, PriceField::High, 1};
    IndicatorEngine engine;
    engine.precompute(raw, {highest});
    EXPECT_FALSE(engine.has("BTC", 20240101, highest));
}

} // namespace
