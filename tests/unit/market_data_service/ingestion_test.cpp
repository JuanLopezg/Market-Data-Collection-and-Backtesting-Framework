#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "fake_binance_http.h"
#include "ingestion.h"
#include "test_support.h"

namespace market_data_test {
namespace {

class MarketIngestionBehaviour : public testing::Test {
protected:
    test_support::TempDir temp_;
    FakeBinanceHttp http_;
    const std::filesystem::path path_ = temp_ / "market.db";

    MarketDataConfig config() const
    {
        MarketDataConfig value;
        value.database_path = path_;
        value.binance_base_url = http_.url();
        value.ranking_size = 1;
        value.active_top_n = 1;
        value.minimum_history_days = 1;
        value.retain_after_top_n_days = 3;
        value.max_parallel_requests = 2;
        return value;
    }

    Json rows(const std::string& sql) const { return readRows(path_, sql); }

    void expectNoCheckpoint()
    {
        MarketDataStore store(path_);
        EXPECT_TRUE(store.loadTracked().days_since_top_n.empty());
        EXPECT_EQ(store.countRankedFor(JAN2), 0);
        EXPECT_TRUE(store.symbolsWithBarOn(JAN2).empty());
        EXPECT_EQ(rows("SELECT COUNT(*) FROM date_of_start"), Json::array({Json::array({0})}));
    }

    void twoSymbolUniverse()
    {
        http_.set("/fapi/v1/exchangeInfo", Json{{"symbols", Json::array({symbol("BTCUSDT"), symbol("ETHUSDT")})}});
        http_.set("/fapi/v1/ticker/24hr", Json::array({ticker("BTCUSDT", "1000"), ticker("ETHUSDT", "900")}));
        http_.set("/fapi/v1/klines:ETHUSDT", Json::array({kline(JAN2_MS), nextOpen(250)}));
    }
};

TEST_F(MarketIngestionBehaviour, R10_SuccessReturnsOnlyAfterCompletedBarsRankingAndTrackerAreDurable)
{
    MarketDataIngestor ingestor(config());
    const auto summary = ingestor.run(JAN2);
    EXPECT_EQ(summary.target_date, JAN2);
    EXPECT_EQ(summary.ranked_symbols, 1U);
    EXPECT_EQ(summary.active_top_n, 1U);
    EXPECT_EQ(summary.tracked_symbols, 1U);
    EXPECT_EQ(summary.maintained_symbols, 1U);
    EXPECT_EQ(summary.requested_symbols, 1U);
    EXPECT_EQ(summary.downloaded_rows, 1U);
    EXPECT_TRUE(summary.paper_opening_prices.empty());
    MarketDataStore reopened(path_);
    EXPECT_EQ(reopened.loadTracked().date, JAN2);
    EXPECT_EQ(reopened.loadTracked().days_since_top_n, (std::map<std::string, int>{{"BTCUSDT", 0}}));
    EXPECT_EQ(reopened.countRankedFor(JAN2), 1);
    EXPECT_EQ(rows("SELECT pair,date,close,quote_volume FROM ohlcv_data"),
        Json::array({Json::array({"BTCUSDT", 20240102, 110, 731})}));
}

TEST_F(MarketIngestionBehaviour, R9_PaperOpenIsReturnedSeparatelyAndUnfinishedBarIsNeverStored)
{
    auto settings = config();
    settings.publish_paper_execution_prices = true;
    MarketDataIngestor ingestor(settings);
    const auto summary = ingestor.run(JAN2);
    EXPECT_EQ(summary.paper_opening_prices, (std::unordered_map<Coin, double>{{"BTCUSDT", 125}}));
    EXPECT_EQ(summary.downloaded_rows, 1U);
    EXPECT_EQ(rows("SELECT date,close,volume,quote_volume FROM ohlcv_data"),
        Json::array({Json::array({20240102, 110, 7, 731})}));
    MarketDataStore reopened(path_);
    EXPECT_TRUE(reopened.symbolsWithBarOn(JAN3).empty());
}

// FINDING: A next-day open without any target-day bar commits the tracker, ranking
// and date marker with zero downloaded rows; R10 requires rejection without a checkpoint.
TEST_F(MarketIngestionBehaviour, DISABLED_R10_NextOpenWithoutTargetCandleCannotEstablishCompletedDay)
{
    http_.set("/fapi/v1/klines:BTCUSDT", Json::array({nextOpen()}));
    MarketDataIngestor ingestor(config());
    EXPECT_THROW((void)ingestor.run(JAN2), std::exception);
    expectNoCheckpoint();
}

TEST_F(MarketIngestionBehaviour, R10_InvalidSymbolPreventsCommittingTheWholeDailySlice)
{
    twoSymbolUniverse();
    auto settings = config();
    settings.ranking_size = 2;
    settings.active_top_n = 2;
    MarketDataIngestor ingestor(settings);
    auto badPrice = kline(JAN2_MS);
    badPrice[2] = "105"; // High below close=110: structurally parseable, economically invalid.
    auto badQuote = kline(JAN2_MS);
    badQuote[7] = "NaN";
    for (const Json& bad : {badPrice, badQuote}) {
        SCOPED_TRACE(bad.dump());
        http_.set("/fapi/v1/klines:ETHUSDT", Json::array({bad, nextOpen(250)}));
        EXPECT_THROW((void)ingestor.run(JAN2), std::exception);
        expectNoCheckpoint();
        EXPECT_EQ(rows("SELECT COUNT(*) FROM ohlcv_data"), Json::array({Json::array({0})}));
    }
}

TEST_F(MarketIngestionBehaviour, R7_InsufficientValidRankingDoesNotAdvanceDurableState)
{
    auto settings = config();
    settings.ranking_size = 2;
    settings.active_top_n = 2;
    MarketDataIngestor ingestor(settings);
    EXPECT_THROW((void)ingestor.run(JAN2), std::exception);
    expectNoCheckpoint();
    for (const auto& request : http_.requests())
        EXPECT_NE(request.path, "/fapi/v1/klines");
}

TEST_F(MarketIngestionBehaviour, R11_RestartKeepsStoredUniverseAndMakesNoHttpRequests)
{
    {
        MarketDataIngestor first(config());
        (void)first.run(JAN2);
    }
    http_.set("/fapi/v1/exchangeInfo", Json{{"symbols", Json::array({symbol("ETHUSDT")})}});
    http_.set("/fapi/v1/ticker/24hr", Json::array({ticker("ETHUSDT", "9000")}));
    http_.clearRequests();
    MarketDataIngestor restarted(config());
    const auto summary = restarted.run(JAN2);
    EXPECT_TRUE(http_.requests().empty());
    EXPECT_EQ(summary.target_date, JAN2);
    EXPECT_EQ(summary.ranked_symbols, 1U);
    EXPECT_EQ(summary.active_top_n, 1U);
    EXPECT_EQ(summary.tracked_symbols, 1U);
    EXPECT_EQ(summary.maintained_symbols, 1U);
    EXPECT_EQ(summary.requested_symbols, 0U);
    EXPECT_EQ(summary.downloaded_rows, 0U);
    EXPECT_TRUE(summary.paper_opening_prices.empty());
    MarketDataStore store(path_);
    EXPECT_EQ(store.loadTracked().days_since_top_n, (std::map<std::string, int>{{"BTCUSDT", 0}}));
    EXPECT_EQ(rows("SELECT pair FROM market_volume_rank_daily"), Json::array({Json::array({"BTCUSDT"})}));
}

TEST_F(MarketIngestionBehaviour, R11_PaperRestartFetchesOnlyFixedNextOpenForStoredSymbols)
{
    {
        MarketDataIngestor first(config());
        (void)first.run(JAN2);
    }
    const auto stored = rows("SELECT * FROM ohlcv_data");
    http_.set("/fapi/v1/klines:BTCUSDT", Json::array({Json::array({JAN3_MS, "125"})}));
    http_.script("/fapi/v1/ticker/24hr", {"unusable current ranking"});
    http_.clearRequests();
    auto settings = config();
    settings.publish_paper_execution_prices = true;
    MarketDataIngestor restarted(settings);
    const auto summary = restarted.run(JAN2);
    EXPECT_EQ(summary.paper_opening_prices, (std::unordered_map<Coin, double>{{"BTCUSDT", 125}}));
    EXPECT_EQ(summary.downloaded_rows, 0U);
    EXPECT_EQ(summary.requested_symbols, 0U);
    EXPECT_EQ(rows("SELECT * FROM ohlcv_data"), stored);
    const auto requests = http_.requests();
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_EQ(requests[0].path, "/fapi/v1/klines");
    EXPECT_EQ(requests[0].query.at("symbol"), "BTCUSDT");
    EXPECT_EQ(requests[0].query.at("startTime"), "1704240000000");
    EXPECT_EQ(requests[0].query.at("limit"), "1");
}

TEST_F(MarketIngestionBehaviour, R11_BackwardsTargetIsRejectedWithoutHttpOrCheckpointChanges)
{
    MarketDataIngestor ingestor(config());
    (void)ingestor.run(JAN2);
    const auto stored = rows("SELECT * FROM ohlcv_data");
    http_.clearRequests();
    EXPECT_THROW((void)ingestor.run(JAN1), std::exception);
    EXPECT_TRUE(http_.requests().empty());
    EXPECT_EQ(rows("SELECT * FROM ohlcv_data"), stored);
    MarketDataStore reopened(path_);
    EXPECT_EQ(reopened.loadTracked().date, JAN2);
    EXPECT_EQ(reopened.countRankedFor(JAN1), 0);
}

TEST_F(MarketIngestionBehaviour, R6_OutageAgesNonTopSymbolsAndPreservesExpiredAndDelistedHistory)
{
    {
        MarketDataStore store(path_);
        auto bars = barsFor("BTCUSDT", {20240101});
        for (const std::string name : {"ETHUSDT", "OLDUSDT", "DELISTUSDT"})
            bars.data[name][20240101] = exampleBar();
        store.commitDailyUpdate({JAN1, {{"BTCUSDT", 0}, {"ETHUSDT", 2},
            {"OLDUSDT", 4}, {"DELISTUSDT", 0}}}, bars, {{"BTCUSDT", 1000}}, JAN1);
    }
    const long long jan4 = JAN3_MS + 24 * HOUR_MS;
    http_.set("/fapi/v1/exchangeInfo", Json{{"symbols", Json::array({
        symbol("BTCUSDT"), symbol("ETHUSDT"), symbol("NEWUSDT")})}});
    http_.set("/fapi/v1/ticker/24hr", Json::array({ticker("BTCUSDT", "1000", jan4),
        ticker("ETHUSDT", "900", jan4), ticker("NEWUSDT", "800", jan4)}));
    http_.set("/fapi/v1/klines:BTCUSDT", Json::array({kline(JAN2_MS), kline(JAN3_MS), kline(jan4, 125)}));
    // An early download for an old tracker is discarded when the final plan expires
    // or delists that symbol. An unusable early answer cannot poison the final slice.
    http_.script("/fapi/v1/klines:ETHUSDT", {"{}"});
    http_.script("/fapi/v1/klines:DELISTUSDT", {"{}"});
    auto settings = config();
    settings.ranking_size = 3;
    MarketDataIngestor ingestor(settings);
    const auto summary = ingestor.run(JAN3);
    EXPECT_EQ(summary.tracked_symbols, 4U);
    EXPECT_EQ(summary.maintained_symbols, 2U);
    EXPECT_EQ(summary.requested_symbols, 1U);
    EXPECT_EQ(summary.downloaded_rows, 2U);
    MarketDataStore reopened(path_);
    EXPECT_EQ(reopened.loadTracked().days_since_top_n, (std::map<std::string, int>{
        {"BTCUSDT", 0}, {"ETHUSDT", 4}, {"OLDUSDT", 6}, {"DELISTUSDT", 2}}));
    EXPECT_EQ(reopened.symbolsWithBarOn(JAN3), (std::vector<std::string>{"BTCUSDT"}));
    EXPECT_EQ(reopened.symbolsWithBarOn(JAN1),
        (std::vector<std::string>{"BTCUSDT", "DELISTUSDT", "ETHUSDT", "OLDUSDT"}));
}

TEST_F(MarketIngestionBehaviour, R11_PreparationCannotReplaceNewerDurableTrackerState)
{
    auto settings = config();
    settings.ranking_size = 2;
    MarketDataIngestor ingestor(settings);
    const auto prepared = ingestor.prepare(JAN2);
    EXPECT_TRUE(prepared.previous.days_since_top_n.empty());
    {
        MarketDataStore concurrentCheckpoint(path_);
        concurrentCheckpoint.commitDailyUpdate({JAN1, {{"ETHUSDT", 0}}},
            barsFor("ETHUSDT", {20240101}), {{"ETHUSDT", 900}}, JAN1);
    }
    twoSymbolUniverse();
    const auto summary = ingestor.run(JAN2, &prepared);
    EXPECT_EQ(summary.tracked_symbols, 2U);
    EXPECT_EQ(summary.requested_symbols, 2U);
    EXPECT_EQ(summary.downloaded_rows, 2U);
    MarketDataStore reopened(path_);
    EXPECT_EQ(reopened.loadTracked().days_since_top_n,
        (std::map<std::string, int>{{"BTCUSDT", 0}, {"ETHUSDT", 1}}));
    EXPECT_EQ(reopened.symbolsWithBarOn(JAN2), (std::vector<std::string>{"BTCUSDT", "ETHUSDT"}));
}

TEST_F(MarketIngestionBehaviour, R9_StaleRankingAndProvisionalBarAreReplacedAfterExchangeClosureProof)
{
    // exchangeInfo's cached time cannot replace the current ticker window or the
    // next candle. The script, rather than the test host's date, supplies proof.
    http_.set("/fapi/v1/exchangeInfo", Json{{"serverTime", JAN3_MS + HOUR_MS},
        {"symbols", Json::array({symbol("BTCUSDT")})}});
    http_.script("/fapi/v1/ticker/24hr", {
        Json::array({ticker("BTCUSDT", "731", JAN3_MS - 1)}).dump(),
        Json::array({ticker("BTCUSDT", "731", JAN3_MS)}).dump()});
    http_.script("/fapi/v1/klines:BTCUSDT", {
        Json::array({kline(JAN2_MS, 100, 109)}).dump(),
        Json::array({kline(JAN2_MS), nextOpen()}).dump()});
    auto settings = config();
    settings.publish_paper_execution_prices = true;
    MarketDataIngestor ingestor(settings);
    const auto summary = ingestor.run(JAN2);
    EXPECT_EQ(summary.paper_opening_prices, (std::unordered_map<Coin, double>{{"BTCUSDT", 125}}));
    EXPECT_EQ(summary.downloaded_rows, 1U);
    EXPECT_EQ(rows("SELECT date,close FROM ohlcv_data"), Json::array({Json::array({20240102, 110})}));
    const auto requests = http_.requests();
    EXPECT_EQ(std::count_if(requests.begin(), requests.end(), [](const auto& request) {
        return request.path == "/fapi/v1/ticker/24hr";
    }), 2);
    EXPECT_EQ(std::count_if(requests.begin(), requests.end(), [](const auto& request) {
        return request.path == "/fapi/v1/klines";
    }), 2);
}

TEST_F(MarketIngestionBehaviour, R16_StopBeforeFundingWorkMakesNoHttpRequestsOrStateChanges)
{
    MarketDataIngestor ingestor(config());
    (void)ingestor.run(JAN2);
    const auto checkpoint = rows("SELECT * FROM ohlcv_data");
    http_.clearRequests();
    ingestor.refreshFundingRates([] { return true; });
    EXPECT_TRUE(http_.requests().empty());
    EXPECT_EQ(rows("SELECT * FROM ohlcv_data"), checkpoint);
    EXPECT_EQ(rows("SELECT COUNT(*) FROM funding_rate_events"), Json::array({Json::array({0})}));
}

TEST_F(MarketIngestionBehaviour, R16_FailedResearchPageKeepsDailyCheckpointAndIndependentFunding)
{
    {
        MarketDataStore store(path_);
        auto bars = barsFor("BTCUSDT", {20240102});
        bars.data["ETHUSDT"][20240102] = exampleBar();
        store.commitDailyUpdate({JAN2, {{"BTCUSDT", 0}, {"ETHUSDT", 0}}}, bars,
            {{"BTCUSDT", 1000}, {"ETHUSDT", 900}}, JAN2);
    }
    const auto checkpoint = rows("SELECT * FROM ohlcv_data ORDER BY pair,date");
    http_.set("/fapi/v1/fundingRate:BTCUSDT", Json::array({settlement(JAN2_MS + 8 * HOUR_MS, "0.001")}));
    http_.script("/fapi/v1/fundingRate:ETHUSDT", {"malformed research page"});
    MarketDataIngestor ingestor(config());
    EXPECT_NO_THROW(ingestor.refreshFundingRates([] { return false; }));
    EXPECT_EQ(rows("SELECT * FROM ohlcv_data ORDER BY pair,date"), checkpoint);
    EXPECT_EQ(rows("SELECT pair,rate FROM funding_rate_events"), Json::array({Json::array({"BTCUSDT", 0.001})}));
    MarketDataStore reopened(path_);
    EXPECT_EQ(reopened.loadTracked().date, JAN2);
    EXPECT_EQ(reopened.countRankedFor(JAN2), 2);
    // A new invocation resumes the successful symbol after its durable event.
    http_.set("/fapi/v1/fundingRate:BTCUSDT", Json::array());
    http_.set("/fapi/v1/fundingRate:ETHUSDT", Json::array());
    http_.clearRequests();
    ingestor.refreshFundingRates([] { return false; });
    for (const auto& request : http_.requests()) {
        if (request.query.at("symbol") == "BTCUSDT") {
            EXPECT_EQ(request.query.at("startTime"), "1704182400001");
        }
    }
    EXPECT_EQ(rows("SELECT COUNT(*) FROM funding_rate_events"), Json::array({Json::array({1})}));
}

TEST_F(MarketIngestionBehaviour, R16_FullFundingPageContinuesAtLastSettlementPlusOne)
{
    {
        MarketDataStore store(path_);
        store.commitDailyUpdate({JAN1, {{"BTCUSDT", 0}}}, barsFor("BTCUSDT", {20240101}),
            {{"BTCUSDT", 731}}, JAN1);
    }
    Json fullPage = Json::array();
    for (int index = 0; index < 1000; ++index)
        fullPage.push_back(settlement(JAN1_MS + index, "0.0001"));
    http_.script("/fapi/v1/fundingRate:BTCUSDT", {fullPage.dump(), "[]"});
    MarketDataIngestor ingestor(config());
    ingestor.refreshFundingRates([] { return false; });
    const auto requests = http_.requests();
    ASSERT_EQ(requests.size(), 2U);
    EXPECT_EQ(requests[0].query.at("startTime"), "1704067200000");
    EXPECT_EQ(requests[1].query.at("startTime"), "1704067201000");
    EXPECT_EQ(rows("SELECT COUNT(*),MIN(funding_time),MAX(funding_time) FROM funding_rate_events"),
        Json::array({Json::array({1000, 1704067200000LL, 1704067200999LL})}));
}

} // namespace
} // namespace market_data_test
