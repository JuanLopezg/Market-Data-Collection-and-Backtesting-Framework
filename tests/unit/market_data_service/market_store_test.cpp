#include <limits>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "market_data_examples.h"
#include "test_support.h"

namespace market_data_test {
namespace {

class MarketStoreBehaviour : public testing::Test {
protected:
    test_support::TempDir temp_;
    const std::filesystem::path path_ = temp_ / "market.db";

    static TrackedMarketData tracker(Date date = JAN2)
    {
        return {date, {{"BTCUSDT", 0}, {"ETHUSDT", 2}}};
    }

    void seed(const OHLCVData& bars, Date date = JAN2)
    {
        MarketDataStore store(path_);
        store.commitDailyUpdate(tracker(date), bars, {{"ETHUSDT", 900}, {"BTCUSDT", 731}}, date);
    }

    Json rows(const std::string& sql) const { return readRows(path_, sql); }

    static void expectRequest(const MarketDataDownloadRequest& request, const std::string& name,
                              Date start, Date end)
    {
        EXPECT_EQ(request.symbol, name);
        EXPECT_EQ(request.start_date, start);
        EXPECT_EQ(request.end_date, end);
    }
};

TEST_F(MarketStoreBehaviour, R2_EmptyDatabaseHasNoCompletedCheckpoint)
{
    MarketDataStore store(path_);
    const auto tracked = store.loadTracked();
    EXPECT_EQ(tracked.date, std::chrono::year{2000}/1/1);
    EXPECT_TRUE(tracked.days_since_top_n.empty());
    EXPECT_EQ(store.countRankedFor(JAN2), 0);
    EXPECT_TRUE(store.symbolsWithBarOn(JAN2).empty());
    EXPECT_TRUE(store.buildFundingPlan().empty());
    EXPECT_EQ(rows("SELECT COUNT(*) FROM funding_rate_daily"), Json::array({Json::array({0})}));
}

TEST_F(MarketStoreBehaviour, R2_ReopeningPreservesPricesTurnoverRankingAndTracker)
{
    seed(barsFor("BTCUSDT", {20240102}));
    MarketDataStore reopened(path_);
    EXPECT_EQ(reopened.loadTracked().date, JAN2);
    EXPECT_EQ(reopened.loadTracked().days_since_top_n, tracker().days_since_top_n);
    EXPECT_EQ(reopened.countRankedFor(JAN2), 2);
    EXPECT_EQ(reopened.symbolsWithBarOn(JAN2), (std::vector<std::string>{"BTCUSDT"}));
    EXPECT_EQ(rows("SELECT pair,date,open,high,low,close,volume,quote_volume FROM ohlcv_data"),
        Json::array({Json::array({"BTCUSDT", 20240102, 100, 120, 90, 110, 7, 731})}));
    EXPECT_EQ(rows("SELECT rank,pair,quote_volume FROM market_volume_rank_daily ORDER BY rank"),
        Json::array({Json::array({1, "ETHUSDT", 900}), Json::array({2, "BTCUSDT", 731})}));
    EXPECT_EQ(rows("SELECT id FROM date_of_start"), Json::array({Json::array({"20240102"})}));
}

TEST_F(MarketStoreBehaviour, R3_DuplicateCheckpointAndPriceCorrectionDoNotAddRows)
{
    const auto bars = barsFor("BTCUSDT", {20240102});
    seed(bars);
    seed(bars);
    EXPECT_EQ(rows("SELECT COUNT(*) FROM ohlcv_data"), Json::array({Json::array({1})}));
    EXPECT_EQ(rows("SELECT COUNT(*) FROM tracked_pairs"), Json::array({Json::array({1})}));
    EXPECT_EQ(rows("SELECT COUNT(*) FROM market_volume_rank_daily"), Json::array({Json::array({2})}));
    EXPECT_EQ(rows("SELECT COUNT(*) FROM date_of_start"), Json::array({Json::array({1})}));
    auto corrected = bars;
    corrected.data.at("BTCUSDT").at(20240102).close = 111;
    seed(corrected);
    EXPECT_EQ(rows("SELECT close,quote_volume FROM ohlcv_data"),
        Json::array({Json::array({111, 731})}));
}

TEST_F(MarketStoreBehaviour, R3_ReplacingOneDaysRankingPreservesEarlierHistory)
{
    seed(barsFor("BTCUSDT", {20240101}), JAN1);
    seed(barsFor("BTCUSDT", {20240102}));
    MarketDataStore store(path_);
    store.commitDailyUpdate(tracker(), {}, {{"BTCUSDT", 731}}, JAN2);
    EXPECT_EQ(store.countRankedFor(JAN1), 2);
    EXPECT_EQ(store.countRankedFor(JAN2), 1);
    EXPECT_EQ(rows("SELECT date,rank,pair FROM market_volume_rank_daily ORDER BY date,rank"),
        Json::array({Json::array({20240101, 1, "ETHUSDT"}), Json::array({20240101, 2, "BTCUSDT"}),
                     Json::array({20240102, 1, "BTCUSDT"})}));
    EXPECT_EQ(store.symbolsWithBarOn(JAN1), (std::vector<std::string>{"BTCUSDT"}));
}

TEST_F(MarketStoreBehaviour, R3_InvalidBarRollsBackBarsRankingTrackerAndDateMarker)
{
    seed(barsFor("BTCUSDT", {20240101}), JAN1);
    std::vector<OHLCV> invalid;
    for (int field = 0; field < 8; ++field) {
        auto bar = exampleBar();
        if (field == 0) bar.open = 0;
        if (field == 1) bar.high = 105;
        if (field == 2) bar.low = 101;
        if (field == 3) bar.close = std::numeric_limits<double>::infinity();
        if (field == 4) bar.volume = -1;
        if (field == 5) bar.quote_volume = -1;
        if (field == 6) bar.quote_volume = std::numeric_limits<double>::infinity();
        if (field == 7) bar.open = std::numeric_limits<double>::quiet_NaN();
        invalid.push_back(bar);
    }
    for (std::size_t index = 0; index < invalid.size(); ++index) {
        SCOPED_TRACE(index);
        auto bars = barsFor("BTCUSDT", {20240102});
        bars.data["ETHUSDT"][20240102] = invalid[index];
        MarketDataStore store(path_);
        EXPECT_THROW(store.commitDailyUpdate(tracker(), bars, {{"BTCUSDT", 731}}, JAN2), std::exception);
        EXPECT_EQ(store.loadTracked().date, JAN1);
        EXPECT_TRUE(store.symbolsWithBarOn(JAN2).empty());
        EXPECT_EQ(store.countRankedFor(JAN2), 0);
        EXPECT_EQ(rows("SELECT id FROM date_of_start"), Json::array({Json::array({"20240101"})}));
    }
}

TEST_F(MarketStoreBehaviour, R3_DuplicateRankedSymbolRollsBackEarlierBarWrites)
{
    seed(barsFor("BTCUSDT", {20240101}), JAN1);
    MarketDataStore store(path_);
    EXPECT_THROW(store.commitDailyUpdate(tracker(), barsFor("BTCUSDT", {20240102}),
        {{"BTCUSDT", 731}, {"BTCUSDT", 900}}, JAN2), std::exception);
    EXPECT_EQ(store.loadTracked().date, JAN1);
    EXPECT_TRUE(store.symbolsWithBarOn(JAN2).empty());
    EXPECT_EQ(store.countRankedFor(JAN1), 2);
    EXPECT_EQ(store.countRankedFor(JAN2), 0);
}

TEST_F(MarketStoreBehaviour, R4_UnknownQuoteTurnoverIsNullAndCannotEraseKnownTurnover)
{
    auto bars = barsFor("BTCUSDT", {20240102});
    bars.data.at("BTCUSDT").at(20240102).quote_volume = std::nan("");
    seed(bars);
    EXPECT_TRUE(rows("SELECT quote_volume FROM ohlcv_data")[0][0].is_null());
    seed(barsFor("BTCUSDT", {20240102}));
    bars.data.at("BTCUSDT").at(20240102).close = 111;
    seed(bars);
    EXPECT_EQ(rows("SELECT close,volume,quote_volume FROM ohlcv_data"),
        Json::array({Json::array({111, 7, 731})}));
}

TEST_F(MarketStoreBehaviour, R4_LegacySchemaGainsNullableQuoteTurnoverWithoutPriceLoss)
{
    Database legacy(path_);
    legacy.exec("CREATE TABLE ohlcv_data(pair TEXT,date INTEGER,open REAL,high REAL,low REAL,"
                "close REAL,volume REAL,PRIMARY KEY(pair,date));"
                "INSERT INTO ohlcv_data VALUES('BTCUSDT',20240102,100,120,90,110,7);");
    MarketDataStore migrated(path_);
    EXPECT_EQ(migrated.symbolsWithBarOn(JAN2), (std::vector<std::string>{"BTCUSDT"}));
    EXPECT_EQ(rows("SELECT close,volume,quote_volume FROM ohlcv_data"),
        Json::array({Json::array({110, 7, nullptr})}));
    const auto plan = migrated.buildDownloadPlan({JAN2, {{"BTCUSDT", 0}}}, JAN2, 3, 1);
    ASSERT_EQ(plan.size(), 1U);
    expectRequest(plan.front(), "BTCUSDT", JAN2, JAN2);
}

TEST_F(MarketStoreBehaviour, R5_NewSymbolBootstrapsInclusiveMinimumHistory)
{
    MarketDataStore store(path_);
    const auto plan = store.buildDownloadPlan({JAN3, {{"BTCUSDT", 0}}}, JAN3, 3, 3);
    ASSERT_EQ(plan.size(), 1U);
    expectRequest(plan.front(), "BTCUSDT", JAN1, JAN3);
}

TEST_F(MarketStoreBehaviour, R5_CatchupRepairsUndersizedWarmupAndThenOnlyForwardDays)
{
    seed(barsFor("BTCUSDT", {20240101}), JAN1);
    MarketDataStore store(path_);
    auto plan = store.buildDownloadPlan({JAN1, {{"BTCUSDT", 0}}}, JAN3, 3, 3);
    ASSERT_EQ(plan.size(), 1U);
    expectRequest(plan.front(), "BTCUSDT", JAN1, JAN3);
    seed(barsFor("BTCUSDT", {20240102, 20240103}), JAN3);
    plan = store.buildDownloadPlan({JAN3, {{"BTCUSDT", 0}}}, JAN5, 3, 3);
    ASSERT_EQ(plan.size(), 1U);
    expectRequest(plan.front(), "BTCUSDT", std::chrono::year{2024}/1/4, JAN5);
}

TEST_F(MarketStoreBehaviour, R5_FillsInteriorGapDespiteHavingEnoughTotalRows)
{
    seed(barsFor("BTCUSDT", {20231231, 20240101, 20240103}), JAN3);
    MarketDataStore store(path_);
    auto plan = store.buildDownloadPlan({JAN3, {{"BTCUSDT", 0}}}, JAN3, 3, 3);
    ASSERT_EQ(plan.size(), 1U);
    expectRequest(plan.front(), "BTCUSDT", JAN2, JAN3);
    seed(barsFor("BTCUSDT", {20240102}), JAN3);
    EXPECT_TRUE(store.buildDownloadPlan({JAN3, {{"BTCUSDT", 0}}}, JAN3, 3, 3).empty());
}

TEST_F(MarketStoreBehaviour, R6_RetentionBoundaryIsInclusiveWithoutDeletingExpiredHistory)
{
    seed(barsFor("OLDUSDT", {20240101}), JAN1);
    MarketDataStore store(path_);
    const auto plan = store.buildDownloadPlan({JAN3, {{"BTCUSDT", 3}, {"OLDUSDT", 4}}}, JAN3, 3, 3);
    ASSERT_EQ(plan.size(), 1U);
    expectRequest(plan.front(), "BTCUSDT", JAN1, JAN3);
    EXPECT_EQ(store.symbolsWithBarOn(JAN1), (std::vector<std::string>{"OLDUSDT"}));
}

TEST_F(MarketStoreBehaviour, R5_BackfillsActualQuoteTurnoverInOtherwiseCompleteHistory)
{
    auto bars = barsFor("BTCUSDT", {20240101, 20240102, 20240103});
    bars.data.at("BTCUSDT").at(20240102).quote_volume = std::nan("");
    seed(bars, JAN3);
    MarketDataStore store(path_);
    const auto plan = store.buildDownloadPlan({JAN3, {{"BTCUSDT", 0}}}, JAN3, 3, 3);
    ASSERT_EQ(plan.size(), 1U);
    expectRequest(plan.front(), "BTCUSDT", JAN2, JAN3);
    seed(barsFor("BTCUSDT", {20240102}), JAN3);
    EXPECT_TRUE(store.buildDownloadPlan({JAN3, {{"BTCUSDT", 0}}}, JAN3, 3, 3).empty());
}

} // namespace
} // namespace market_data_test
