#include <gtest/gtest.h>

#include "data_types.h"
#include "price_snapshot.h"
#include "rolling_market_state.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

constexpr double missing = std::numeric_limits<double>::quiet_NaN();
constexpr OHLCV btc{100, 120, 90, 110, 3, 321};
constexpr OHLCV eth{20, 24, 18, 22, 5, 101};

MarketSliceSnapshot slice(Timestamp date, std::initializer_list<MarketBarSnapshot> bars)
{
    MarketSliceSnapshot result;
    result.timestamp = date;
    result.bars = bars;
    return result;
}

template<typename Bar>
void expectBar(const Bar& actual, const OHLCV& expected)
{
    EXPECT_DOUBLE_EQ(actual.open, expected.open);
    EXPECT_DOUBLE_EQ(actual.high, expected.high);
    EXPECT_DOUBLE_EQ(actual.low, expected.low);
    EXPECT_DOUBLE_EQ(actual.close, expected.close);
    EXPECT_DOUBLE_EQ(actual.volume, expected.volume);
    if (std::isnan(expected.quote_volume)) EXPECT_TRUE(std::isnan(actual.quote_volume));
    else EXPECT_DOUBLE_EQ(actual.quote_volume, expected.quote_volume);
}

TEST(MarketBars, R1_ReshapePreservesEverySourceFieldAndAsset)
{
    OHLCVData raw;
    raw.data["BTC"][20240301] = btc;
    raw.data["ETH"][20240301] = eth;
    const auto market = buildMarketData(raw);
    ASSERT_EQ(market.size(), 1u);
    ASSERT_EQ(market.at(20240301).size(), 2u);
    expectBar(market.at(20240301).at("BTC"), btc);
    expectBar(market.at(20240301).at("ETH"), eth);
    // Source base volume 3 and actual quote turnover 321 stay independent of 3*110.
    EXPECT_DOUBLE_EQ(market.at(20240301).at("BTC").quote_volume, 321);
}

TEST(MarketBars, R1_EmptyAndMissingHistoryDoNotCreateBars)
{
    EXPECT_TRUE(buildMarketData(OHLCVData{}).empty());
    OHLCVData raw;
    raw.data["BTC"][20240228] = btc;
    raw.data["BTC"][20240301] = btc;
    raw.data["ETH"][20240301] = eth;
    const auto market = buildMarketData(raw);
    ASSERT_EQ(market.size(), 2u);
    EXPECT_FALSE(market.contains(20240229));
    EXPECT_FALSE(market.at(20240228).contains("ETH"));
}

TEST(MarketBars, R2_UnknownAndZeroQuoteTurnoverAreNeverEstimated)
{
    OHLCVData raw;
    raw.data["BTC"][20240228] = {100, 120, 90, 110, 3};
    raw.data["BTC"][20240229] = {100, 120, 90, 110, 3, 0};
    raw.data["BTC"][20240301] = btc;
    const auto market = buildMarketData(raw);
    EXPECT_TRUE(std::isnan(market.at(20240228).at("BTC").quote_volume));
    EXPECT_DOUBLE_EQ(market.at(20240229).at("BTC").quote_volume, 0);
    EXPECT_DOUBLE_EQ(market.at(20240301).at("BTC").quote_volume, 321);
    EXPECT_TRUE(std::isnan(raw.data.at("BTC").at(20240228).quote_volume));
}

TEST(MarketBars, R3_BarNumbersFollowEachAssetsChronologyAcrossGaps)
{
    OHLCVData raw;
    // Deliberately insert out of chronological order; map keys define observation order.
    raw.data["BTC"][20240301] = btc;
    raw.data["BTC"][20240228] = btc;
    raw.data["BTC"][20240229] = btc;
    raw.data["ETH"][20240301] = eth;
    raw.data["ETH"][20240228] = eth;
    const auto market = buildMarketData(raw);
    EXPECT_EQ(market.at(20240228).at("BTC").barNumber, 1u);
    EXPECT_EQ(market.at(20240229).at("BTC").barNumber, 2u);
    EXPECT_EQ(market.at(20240301).at("BTC").barNumber, 3u);
    EXPECT_EQ(market.at(20240301).at("ETH").barNumber, 2u);
}

TEST(ReferencePrices, R7_UnknownAssetsCannotSubstituteZeroOrAnotherPrice)
{
    PriceSnapshot prices;
    prices.set("BTC", 110);
    EXPECT_THROW(prices.get("ETH"), std::out_of_range);
    EXPECT_FALSE(prices.contains("ETH"));
    EXPECT_DOUBLE_EQ(prices.get("BTC"), 110);
}

TEST(ReferencePrices, R7_InvalidPricesCannotInsertOrReplaceAnAcceptedReference)
{
    PriceSnapshot prices;
    prices.set("BTC", 110);
    for (double value : {0.0, -1.0, missing, std::numeric_limits<double>::infinity()}) {
        EXPECT_THROW(prices.set("BTC", value), std::invalid_argument);
        EXPECT_THROW(prices.set("ETH", value), std::invalid_argument);
        EXPECT_DOUBLE_EQ(prices.get("BTC"), 110);
        EXPECT_FALSE(prices.contains("ETH"));
        EXPECT_EQ(prices.size(), 1u);
    }
}

TEST(RollingMarket, R4_CompleteSliceMakesBothAssetsVisibleTogether)
{
    RollingMarketState state;
    ASSERT_TRUE(state.append(slice(20240301, {{"BTC", btc}, {"ETH", eth}})));
    ASSERT_EQ(state.marketData().at(20240301).size(), 2u);
    expectBar(state.marketData().at(20240301).at("BTC"), btc);
    expectBar(state.rawData().data.at("ETH").at(20240301), eth);
    EXPECT_EQ(state.latestTimestamp(), 20240301u);
}

TEST(RollingMarket, R4_InvalidSliceIdentityOrMembershipLeavesStateEmpty)
{
    RollingMarketState state;
    const std::vector<MarketSliceSnapshot> invalid{
        slice(0, {{"BTC", btc}}), slice(20240301, {}),
        slice(20240301, {{"", btc}}), slice(20240301, {{"BTC", btc}, {"BTC", btc}})};
    for (const auto& candidate : invalid) {
        EXPECT_THROW(state.append(candidate), std::invalid_argument);
        EXPECT_TRUE(state.empty());
        EXPECT_TRUE(state.rawData().data.empty());
        EXPECT_TRUE(state.marketData().empty());
    }
}

TEST(RollingMarket, R4_NonpositiveOrNonfinitePricesAreRejected)
{
    for (double OHLCV::* field : {&OHLCV::open, &OHLCV::high, &OHLCV::low, &OHLCV::close}) {
        for (double value : {0.0, -1.0, missing, std::numeric_limits<double>::infinity()}) {
            RollingMarketState state;
            auto invalid = btc;
            invalid.*field = value;
            EXPECT_THROW(state.append(slice(20240301, {{"BTC", invalid}})), std::invalid_argument);
            EXPECT_TRUE(state.marketData().empty());
        }
    }
}

TEST(RollingMarket, R4_NegativeOrNonfiniteBaseVolumeIsRejected)
{
    for (double value : {-1.0, missing, std::numeric_limits<double>::infinity()}) {
        RollingMarketState state;
        auto invalid = btc;
        invalid.volume = value;
        EXPECT_THROW(state.append(slice(20240301, {{"BTC", invalid}})), std::invalid_argument);
        EXPECT_TRUE(state.empty());
    }
}

TEST(RollingMarket, R2_InvalidSuppliedQuoteTurnoverIsRejected)
{
    for (double value : {-1.0, std::numeric_limits<double>::infinity()}) {
        RollingMarketState state;
        auto invalid = btc;
        invalid.quote_volume = value;
        EXPECT_THROW(state.append(slice(20240301, {{"BTC", invalid}})), std::invalid_argument);
        EXPECT_TRUE(state.empty());
    }
}

TEST(RollingMarket, R4_HighAndLowMustEncloseOpenAndClose)
{
    for (const OHLCV invalid : {OHLCV{100, 105, 90, 110, 3, 321},
                               OHLCV{100, 120, 105, 110, 3, 321}}) {
        RollingMarketState state;
        EXPECT_THROW(state.append(slice(20240301, {{"BTC", invalid}})), std::invalid_argument);
        EXPECT_TRUE(state.marketData().empty());
    }
}

TEST(RollingMarket, R4_BadSecondBarCannotPartiallyAdvanceHistoryAndRetryCountsOnce)
{
    RollingMarketState state;
    ASSERT_TRUE(state.append(slice(20240229, {{"BTC", btc}})));
    auto invalid = eth;
    invalid.close = 0;
    EXPECT_THROW(state.append(slice(20240301, {{"BTC", btc}, {"ETH", invalid}})),
                 std::invalid_argument);
    EXPECT_EQ(state.latestTimestamp(), 20240229u);
    EXPECT_FALSE(state.marketData().contains(20240301));
    EXPECT_FALSE(state.rawData().data.at("BTC").contains(20240301));
    EXPECT_FALSE(state.rawData().data.contains("ETH"));
    ASSERT_TRUE(state.append(slice(20240301, {{"BTC", btc}, {"ETH", eth}})));
    EXPECT_EQ(state.marketData().at(20240301).at("BTC").barNumber, 2u);
    EXPECT_EQ(state.marketData().at(20240301).at("ETH").barNumber, 1u);
}

TEST(RollingMarket, R4_ZeroBaseAndQuoteVolumeAreValidObservations)
{
    RollingMarketState state;
    ASSERT_TRUE(state.append(slice(20240301, {{"BTC", {100, 120, 90, 110, 0, 0}}})));
    EXPECT_DOUBLE_EQ(state.marketData().at(20240301).at("BTC").volume, 0);
    EXPECT_DOUBLE_EQ(state.rawData().data.at("BTC").at(20240301).quote_volume, 0);
}

TEST(RollingMarket, R5_IdenticalLatestRedeliveryDoesNotIncrementHistory)
{
    RollingMarketState state;
    const auto first = slice(20240229, {{"BTC", btc}, {"ETH", eth}});
    ASSERT_TRUE(state.append(first));
    EXPECT_FALSE(state.append(first));
    EXPECT_EQ(state.marketData().size(), 1u);
    EXPECT_EQ(state.rawData().data.at("BTC").size(), 1u);
    ASSERT_TRUE(state.append(slice(20240301, {{"BTC", btc}})));
    EXPECT_EQ(state.marketData().at(20240301).at("BTC").barNumber, 2u);
}

TEST(RollingMarket, R5_RedeliveryIgnoresVectorOrderAndTreatsMissingQuoteAsEqual)
{
    RollingMarketState state;
    auto unknown = btc;
    unknown.quote_volume = missing;
    ASSERT_TRUE(state.append(slice(20240301, {{"BTC", unknown}, {"ETH", eth}})));
    EXPECT_FALSE(state.append(slice(20240301, {{"ETH", eth}, {"BTC", unknown}})));
    EXPECT_EQ(state.rawData().data.at("BTC").size(), 1u);
    EXPECT_TRUE(std::isnan(state.marketData().at(20240301).at("BTC").quote_volume));
}

TEST(RollingMarket, R5_ConflictingBarFieldsCannotOverwriteTheLatestSlice)
{
    RollingMarketState state;
    ASSERT_TRUE(state.append(slice(20240301, {{"BTC", btc}})));
    for (double OHLCV::* field : {&OHLCV::open, &OHLCV::high, &OHLCV::low,
                                  &OHLCV::close, &OHLCV::volume, &OHLCV::quote_volume}) {
        auto changed = btc;
        changed.*field += 1;
        EXPECT_THROW(state.append(slice(20240301, {{"BTC", changed}})), std::logic_error);
        expectBar(state.rawData().data.at("BTC").at(20240301), btc);
        EXPECT_EQ(state.marketData().at(20240301).at("BTC").barNumber, 1u);
    }
}

TEST(RollingMarket, R5_ChangedQuoteAvailabilityIsAConflictInEitherDirection)
{
    auto unknown = btc;
    unknown.quote_volume = missing;
    RollingMarketState knownState;
    ASSERT_TRUE(knownState.append(slice(20240301, {{"BTC", btc}})));
    EXPECT_THROW(knownState.append(slice(20240301, {{"BTC", unknown}})), std::logic_error);
    EXPECT_DOUBLE_EQ(knownState.rawData().data.at("BTC").at(20240301).quote_volume, 321);
    RollingMarketState unknownState;
    ASSERT_TRUE(unknownState.append(slice(20240301, {{"BTC", unknown}})));
    EXPECT_THROW(unknownState.append(slice(20240301, {{"BTC", btc}})), std::logic_error);
    EXPECT_TRUE(std::isnan(unknownState.rawData().data.at("BTC").at(20240301).quote_volume));
}

TEST(RollingMarket, R5_ChangedAssetSetIsAConflictEvenWithTheSameNumberOfBars)
{
    RollingMarketState state;
    ASSERT_TRUE(state.append(slice(20240301, {{"BTC", btc}, {"ETH", eth}})));
    EXPECT_THROW(state.append(slice(20240301, {{"BTC", btc}})), std::logic_error);
    EXPECT_THROW(state.append(slice(20240301, {{"BTC", btc}, {"HELD", eth}})), std::logic_error);
    EXPECT_EQ(state.marketData().at(20240301).size(), 2u);
    EXPECT_TRUE(state.marketData().at(20240301).contains("ETH"));
    EXPECT_FALSE(state.rawData().data.contains("HELD"));
}

TEST(RollingMarket, R6_OlderRedeliveryCannotMoveTheFrontierBackwards)
{
    RollingMarketState state;
    const auto old = slice(20240229, {{"BTC", btc}});
    ASSERT_TRUE(state.append(old));
    ASSERT_TRUE(state.append(slice(20240301, {{"BTC", btc}})));
    EXPECT_THROW(state.append(old), std::logic_error);
    EXPECT_EQ(state.latestTimestamp(), 20240301u);
    EXPECT_EQ(state.rawData().data.at("BTC").size(), 2u);
}

TEST(RollingMarket, R6_UnreleasedFutureBarsAndPricesAreNotVisible)
{
    RollingMarketState state;
    ASSERT_TRUE(state.append(slice(20240301, {{"BTC", btc}})));
    EXPECT_FALSE(state.marketData().contains(20240302));
    EXPECT_FALSE(state.rawData().data.at("BTC").contains(20240302));
    EXPECT_THROW(state.openPrices(20240302), std::out_of_range);
    EXPECT_THROW(state.closePrices(20240302), std::out_of_range);
}

TEST(RollingMarket, R6_MissingAssetsAreNotForwardFilledIntoTheNextSlice)
{
    RollingMarketState state;
    ASSERT_TRUE(state.append(slice(20240229, {{"BTC", btc}, {"ETH", eth}})));
    ASSERT_TRUE(state.append(slice(20240301, {{"BTC", btc}})));
    EXPECT_FALSE(state.marketData().at(20240301).contains("ETH"));
    EXPECT_FALSE(state.rawData().data.at("ETH").contains(20240301));
    EXPECT_THROW(state.closePrices(20240301).get("ETH"), std::out_of_range);
}

TEST(RollingMarket, R3_PerAssetCountsDoNotAdvanceForMissingDates)
{
    RollingMarketState state;
    ASSERT_TRUE(state.append(slice(20240228, {{"BTC", btc}, {"ETH", eth}})));
    ASSERT_TRUE(state.append(slice(20240229, {{"BTC", btc}})));
    ASSERT_TRUE(state.append(slice(20240301, {{"BTC", btc}, {"ETH", eth}})));
    EXPECT_EQ(state.marketData().at(20240301).at("BTC").barNumber, 3u);
    EXPECT_EQ(state.marketData().at(20240301).at("ETH").barNumber, 2u);
}

TEST(RollingMarket, R6_ReplayingAcceptedPrefixThenContinuingMatchesUninterruptedState)
{
    const std::vector<MarketSliceSnapshot> messages{
        slice(20240228, {{"BTC", btc}, {"ETH", eth}}),
        slice(20240229, {{"BTC", btc}}),
        slice(20240301, {{"BTC", btc}, {"ETH", eth}})};
    RollingMarketState uninterrupted;
    for (const auto& message : messages) ASSERT_TRUE(uninterrupted.append(message));
    RollingMarketState restarted;
    ASSERT_TRUE(restarted.append(messages[0]));
    ASSERT_TRUE(restarted.append(messages[1]));
    EXPECT_FALSE(restarted.append(messages[1]));
    ASSERT_TRUE(restarted.append(messages[2]));
    ASSERT_EQ(restarted.marketData().size(), uninterrupted.marketData().size());
    ASSERT_EQ(restarted.rawData().data.size(), uninterrupted.rawData().data.size());
    for (const auto& [date, assets] : uninterrupted.marketData()) {
        ASSERT_EQ(restarted.marketData().at(date).size(), assets.size());
        for (const auto& [coin, bar] : assets) {
            expectBar(restarted.marketData().at(date).at(coin),
                      uninterrupted.rawData().data.at(coin).at(date));
            EXPECT_EQ(restarted.marketData().at(date).at(coin).barNumber, bar.barNumber);
        }
    }
    EXPECT_EQ(restarted.latestTimestamp(), 20240301u);
    EXPECT_EQ(restarted.marketData().at(20240301).at("ETH").barNumber, 2u);
}

TEST(RollingMarket, R7_OpenAndClosePricesUseTheRequestedDateAndPriceField)
{
    RollingMarketState state;
    ASSERT_TRUE(state.append(slice(20240301, {{"BTC", btc}, {"ETH", eth}})));
    const auto opens = state.openPrices(20240301);
    const auto closes = state.closePrices(20240301);
    EXPECT_DOUBLE_EQ(opens.get("BTC"), 100);
    EXPECT_DOUBLE_EQ(closes.get("BTC"), 110);
    EXPECT_DOUBLE_EQ(opens.get("ETH"), 20);
    EXPECT_DOUBLE_EQ(closes.get("ETH"), 22);
    EXPECT_THROW(state.closePrices(20240229), std::out_of_range);
}

TEST(RollingMarket, R7_PreviouslyObtainedCloseSnapshotSurvivesLaterAppends)
{
    RollingMarketState state;
    ASSERT_TRUE(state.append(slice(20240301, {{"BTC", btc}})));
    const auto completed = state.closePrices(20240301);
    ASSERT_TRUE(state.append(slice(20240302, {{"BTC", {9000, 10010, 8990, 10000, 3, 30000}}})));
    EXPECT_DOUBLE_EQ(completed.get("BTC"), 110);
    EXPECT_DOUBLE_EQ(state.closePrices(20240301).get("BTC"), 110);
    EXPECT_DOUBLE_EQ(state.closePrices(20240302).get("BTC"), 10000);
}

} // namespace
