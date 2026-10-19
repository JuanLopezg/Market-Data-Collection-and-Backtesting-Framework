#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "binance_client.h"
#include "fake_binance_http.h"

namespace market_data_test {
namespace {

TEST(BinanceMarketDataBehaviour, R7_RanksOnlyEligiblePerpetualsByActualTurnoverWithStableTies)
{
    FakeBinanceHttp http;
    http.set("/fapi/v1/exchangeInfo", Json{{"symbols", Json::array({
        symbol("BTCUSDT"), symbol("ETHUSDT"), symbol("XUSDC", "TRADING", "USDC"),
        symbol("HALTUSDT", "BREAK"), symbol("DATEDUSDT", "TRADING", "USDT", "CURRENT_QUARTER"),
        symbol("BADUSDT"), Json{{"symbol", "BROKEN"}}})}});
    http.set("/fapi/v1/ticker/24hr", Json::array({
        ticker("BTCUSDT", "731"), ticker("ETHUSDT", "900"), ticker("XUSDC", "5000"),
        ticker("HALTUSDT", "9000"), ticker("DATEDUSDT", "9999"), ticker("BADUSDT", "NaN")}));
    BinanceMarketDataClient client(http.url());
    auto universe = client.universeByQuoteVolume(2);
    EXPECT_EQ(universe.ranked_top, (std::vector<std::pair<std::string, double>>{
        {"ETHUSDT", 900}, {"BTCUSDT", 731}}));
    EXPECT_EQ(universe.eligible_symbols, (std::set<std::string>{"BADUSDT", "BTCUSDT", "ETHUSDT"}));
    http.set("/fapi/v1/ticker/24hr", Json::array({ticker("ETHUSDT", "900"), ticker("BTCUSDT", "900"),
        ticker("BADUSDT", "-1")}));
    universe = client.universeByQuoteVolume(1);
    EXPECT_EQ(universe.ranked_top, (std::vector<std::pair<std::string, double>>{{"BTCUSDT", 900}}));
}

TEST(BinanceMarketDataBehaviour, R7_UnusableUniverseDocumentsCannotSupplyARanking)
{
    FakeBinanceHttp http;
    BinanceMarketDataClient client(http.url());
    for (const std::string bad : {"{", "[]", "{\"symbols\":{}}"}) {
        SCOPED_TRACE(bad);
        http.script("/fapi/v1/exchangeInfo", {bad});
        EXPECT_TRUE(client.universeByQuoteVolume(2).ranked_top.empty());
    }
    http.set("/fapi/v1/exchangeInfo", Json{{"symbols", Json::array({symbol("BTCUSDT")})}});
    for (const std::string bad : {"{", "{}"}) {
        SCOPED_TRACE(bad);
        http.script("/fapi/v1/ticker/24hr", {bad});
        EXPECT_TRUE(client.universeByQuoteVolume(2).ranked_top.empty());
    }
}

TEST(BinanceMarketDataBehaviour, R8_CompletedBarsUseRequestedUtcDatesAndSourceQuoteTurnover)
{
    FakeBinanceHttp http;
    http.set("/fapi/v1/klines:BTCUSDT", Json::array({kline(JAN1_MS), kline(JAN2_MS), kline(JAN3_MS)}));
    BinanceMarketDataClient client(http.url());
    const auto fetched = client.fetchDailyBars({{"BTCUSDT", JAN2, JAN2}}, 2);
    ASSERT_TRUE(fetched.failed_symbols.empty());
    ASSERT_EQ(fetched.bars.data.size(), 1U);
    const auto& byDay = fetched.bars.data.at("BTCUSDT");
    ASSERT_EQ(byDay.size(), 1U);
    ASSERT_TRUE(byDay.contains(20240102));
    const auto& bar = byDay.at(20240102);
    EXPECT_DOUBLE_EQ(bar.open, 100);
    EXPECT_DOUBLE_EQ(bar.high, 120);
    EXPECT_DOUBLE_EQ(bar.low, 90);
    EXPECT_DOUBLE_EQ(bar.close, 110);
    EXPECT_DOUBLE_EQ(bar.volume, 7);
    EXPECT_DOUBLE_EQ(bar.quote_volume, 731);
    const auto requests = http.requests();
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_EQ(requests.front().query.at("startTime"), "1704153600000");
    EXPECT_EQ(requests.front().query.at("endTime"), "1704239999999");
    EXPECT_EQ(requests.front().query.at("interval"), "1d");
}

TEST(BinanceMarketDataBehaviour, R9_NextDayOpenNeverExposesUnfinishedHlcOrVolume)
{
    FakeBinanceHttp http;
    BinanceMarketDataClient client(http.url());
    const auto snapshot = client.fetchDailySnapshot({{"BTCUSDT", JAN2, JAN2}}, false, 1, 2);
    EXPECT_FALSE(snapshot.universe.has_value());
    ASSERT_TRUE(snapshot.candles.failed_symbols.empty());
    ASSERT_EQ(snapshot.candles.bars.data.at("BTCUSDT").size(), 1U);
    EXPECT_TRUE(snapshot.candles.bars.data.at("BTCUSDT").contains(20240102));
    EXPECT_FALSE(snapshot.candles.bars.data.at("BTCUSDT").contains(20240103));
    EXPECT_EQ(snapshot.candles.next_opens, (std::unordered_map<Coin, double>{{"BTCUSDT", 125}}));
}

TEST(BinanceMarketDataBehaviour, R8_InvalidSymbolRequestDiscardsPartialBarsButPreservesOtherSymbols)
{
    FakeBinanceHttp http;
    http.set("/fapi/v1/klines:ETHUSDT", Json::array({kline(JAN2_MS)}));
    BinanceMarketDataClient client(http.url());
    auto invalidQuote = kline(JAN2_MS);
    invalidQuote[7] = "NaN";
    for (const std::string& bad : {std::string("{"), std::string("{}"),
            Json::array({kline(JAN2_MS), Json::array({JAN2_MS})}).dump(),
            Json::array({kline(JAN2_MS), invalidQuote}).dump()}) {
        SCOPED_TRACE(bad);
        http.script("/fapi/v1/klines:BTCUSDT", {bad});
        const auto fetched = client.fetchDailyBars({{"BTCUSDT", JAN2, JAN2}, {"ETHUSDT", JAN2, JAN2}}, 2);
        EXPECT_EQ(fetched.failed_symbols, (std::vector<std::string>{"BTCUSDT"}));
        EXPECT_FALSE(fetched.bars.data.contains("BTCUSDT"));
        ASSERT_TRUE(fetched.bars.data.contains("ETHUSDT"));
        EXPECT_DOUBLE_EQ(fetched.bars.data.at("ETHUSDT").at(20240102).quote_volume, 731);
    }
}

TEST(BinanceMarketDataBehaviour, R12_OpeningSnapshotReadsOnlyExactDateAndOpenForEverySymbol)
{
    FakeBinanceHttp http;
    http.set("/fapi/v1/klines:BTCUSDT", Json::array({Json::array({JAN3_MS, "125"})}));
    http.set("/fapi/v1/klines:ETHUSDT", Json::array({Json::array({JAN3_MS, "250"})}));
    BinanceMarketDataClient client(http.url());
    EXPECT_EQ(client.fetchOpeningPrices(JAN3, {"BTCUSDT", "ETHUSDT"}, 2),
        (std::unordered_map<Coin, double>{{"BTCUSDT", 125}, {"ETHUSDT", 250}}));
}

TEST(BinanceMarketDataBehaviour, R12_MisdatedOrInvalidOpenRejectsTheWholePortfolioSnapshot)
{
    FakeBinanceHttp http;
    http.set("/fapi/v1/klines:BTCUSDT", Json::array({Json::array({JAN3_MS, "125"})}));
    BinanceMarketDataClient client(http.url());
    for (const Json& row : {Json::array({JAN2_MS, "250"}), Json::array({JAN3_MS + 1, "250"}),
            Json::array({JAN3_MS, "0"}), Json::array({JAN3_MS, "-1"}),
            Json::array({JAN3_MS, "NaN"}), Json::array({JAN3_MS, "inf"})}) {
        SCOPED_TRACE(row.dump());
        http.set("/fapi/v1/klines:ETHUSDT", Json::array({row}));
        EXPECT_THROW((void)client.fetchOpeningPrices(JAN3, {"BTCUSDT", "ETHUSDT"}, 2), std::exception);
    }
}

TEST(BinanceMarketDataBehaviour, R16_FundingPagesPreserveSignedRatesAndOptionalMarksInRequestOrder)
{
    FakeBinanceHttp http;
    auto missingMark = settlement(1000, "0.001");
    missingMark.erase("markPrice");
    http.set("/fapi/v1/fundingRate:BTCUSDT", Json::array({missingMark, settlement(2000, "-0.002", "")}));
    http.set("/fapi/v1/fundingRate:ETHUSDT", Json::array({settlement(3000, "0.003", "250")}));
    BinanceMarketDataClient client(http.url());
    const auto pages = client.fetchFundingRatePages({{"ETHUSDT", 3000}, {"BTCUSDT", 1000}}, 2);
    ASSERT_EQ(pages.size(), 2U);
    ASSERT_FALSE(pages[0].failed);
    ASSERT_FALSE(pages[1].failed);
    ASSERT_EQ(pages[0].events.size(), 1U);
    ASSERT_EQ(pages[1].events.size(), 2U);
    EXPECT_EQ(pages[0].events[0].funding_time_ms, 3000);
    EXPECT_DOUBLE_EQ(pages[0].events[0].rate, 0.003);
    EXPECT_DOUBLE_EQ(pages[0].events[0].mark_price, 250);
    EXPECT_EQ(pages[1].events[0].funding_time_ms, 1000);
    EXPECT_DOUBLE_EQ(pages[1].events[0].rate, 0.001);
    EXPECT_DOUBLE_EQ(pages[1].events[1].rate, -0.002);
    EXPECT_TRUE(std::isnan(pages[1].events[0].mark_price));
    EXPECT_TRUE(std::isnan(pages[1].events[1].mark_price));
    for (const auto& request : http.requests())
        EXPECT_EQ(request.query.at("limit"), "1000");
}

TEST(BinanceMarketDataBehaviour, R16_MalformedUnorderedDuplicateOrPreStartEventsFailTheWholePage)
{
    FakeBinanceHttp http;
    BinanceMarketDataClient client(http.url());
    for (const std::string& bad : {std::string("{"), std::string("{}"),
            Json::array({settlement(2000, "0.001"), settlement(1000, "0.002")}).dump(),
            Json::array({settlement(1000, "0.001"), settlement(1000, "0.002")}).dump(),
            Json::array({settlement(999, "0.001")}).dump(),
            Json::array({settlement(1000, "0.001"), settlement(2000, "NaN")}).dump()}) {
        SCOPED_TRACE(bad);
        http.script("/fapi/v1/fundingRate:BTCUSDT", {bad});
        const auto pages = client.fetchFundingRatePages({{"BTCUSDT", 1000}}, 2);
        ASSERT_EQ(pages.size(), 1U);
        EXPECT_TRUE(pages.front().failed);
        EXPECT_TRUE(pages.front().events.empty());
    }
}

TEST(BinanceMarketDataBehaviour, R8_ThousandDaySnapshotKeepsEveryRequestWithinBinancePageLimit)
{
    FakeBinanceHttp http;
    BinanceMarketDataClient client(http.url());
    const Date first{std::chrono::sys_days{JAN2} - std::chrono::days{999}};
    const auto snapshot = client.fetchDailySnapshot({{"BTCUSDT", first, JAN2}}, false, 1, 2);
    EXPECT_TRUE(snapshot.candles.failed_symbols.empty());
    // 1000 completed days + one opening candle cannot fit in a 1000-row page.
    const auto requests = http.requests();
    EXPECT_GE(requests.size(), 2U);
    for (const auto& request : requests) {
        ASSERT_EQ(request.path, "/fapi/v1/klines");
        EXPECT_LE(std::stoll(request.query.at("limit")), 1000);
    }
}

} // namespace
} // namespace market_data_test
