#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "market_data_examples.h"
#include "test_support.h"

namespace market_data_test {
namespace {

class FundingStoreBehaviour : public testing::Test {
protected:
    test_support::TempDir temp_;
    const std::filesystem::path path_ = temp_ / "market.db";

    Json rows(const std::string& sql) const { return readRows(path_, sql); }

    void seedBars(const OHLCVData& bars)
    {
        MarketDataStore store(path_);
        store.commitDailyUpdate({JAN2, {{"BTCUSDT", 0}}}, bars, {{"BTCUSDT", 731}}, JAN2);
    }
};

TEST_F(FundingStoreBehaviour, R13_SignedSettlementsSurviveReopenAndDuplicateWithoutLosingKnownMark)
{
    {
        MarketDataStore store(path_);
        store.storeFundingRates({{"BTCUSDT", {{JAN2_MS + 8 * HOUR_MS, 0.001, 100},
                                              {JAN2_MS + 16 * HOUR_MS, -0.002, std::nan("")}}}});
        store.storeFundingRates({{"BTCUSDT", {{JAN2_MS + 8 * HOUR_MS, 0.001, std::nan("")}}}});
    }
    MarketDataStore reopened(path_);
    const auto persisted = rows("SELECT funding_time,rate,mark_price FROM funding_rate_events ORDER BY funding_time");
    ASSERT_EQ(persisted.size(), 2U);
    EXPECT_EQ(persisted[0][0].get<long long>(), 1704182400000LL);
    EXPECT_DOUBLE_EQ(persisted[0][1].get<double>(), 0.001);
    EXPECT_DOUBLE_EQ(persisted[0][2].get<double>(), 100);
    EXPECT_DOUBLE_EQ(persisted[1][1].get<double>(), -0.002);
    EXPECT_TRUE(persisted[1][2].is_null());
    EXPECT_DOUBLE_EQ(1000 * persisted[0][1].get<double>(), 1);
    EXPECT_DOUBLE_EQ(1000 * persisted[1][1].get<double>(), -2);
}

TEST_F(FundingStoreBehaviour, R14_EightHourFundingIncludesClosingMidnightAndExcludesOpeningMidnight)
{
    MarketDataStore store(path_);
    store.storeFundingRates({{"BTCUSDT", {
        {JAN2_MS, 0.004, 100}, {JAN2_MS + 8 * HOUR_MS, 0.001, 100},
        {JAN2_MS + 16 * HOUR_MS, -0.002, 100}, {JAN3_MS, 0.003, 100}}}});
    const auto daily = rows("SELECT date,funding_rate_sum,funding_rate_mean,events "
                            "FROM funding_rate_daily ORDER BY date");
    ASSERT_EQ(daily.size(), 2U);
    EXPECT_EQ(daily[0][0], 20240101);
    EXPECT_DOUBLE_EQ(daily[0][1].get<double>(), 0.004);
    EXPECT_EQ(daily[0][3], 1);
    EXPECT_EQ(daily[1][0], 20240102);
    EXPECT_NEAR(daily[1][1].get<double>(), 0.002, 1e-15);
    EXPECT_NEAR(daily[1][2].get<double>(), 0.002 / 3, 1e-15);
    EXPECT_EQ(daily[1][3], 3);
}

TEST_F(FundingStoreBehaviour, R14_FourHourDayAndNewestIncompleteDayExposeActualEventCounts)
{
    MarketDataStore store(path_);
    std::vector<FundingRateEvent> events;
    for (int hour : {4, 8, 12, 16, 20, 24, 28})
        events.push_back({JAN2_MS + hour * HOUR_MS, 0.001, 100});
    store.storeFundingRates({{"BTCUSDT", events}});
    const auto daily = rows("SELECT date,funding_rate_sum,funding_rate_mean,events "
                            "FROM funding_rate_daily ORDER BY date");
    ASSERT_EQ(daily.size(), 2U);
    EXPECT_EQ(daily[0][0], 20240102);
    EXPECT_NEAR(daily[0][1].get<double>(), 0.006, 1e-15);
    EXPECT_NEAR(daily[0][2].get<double>(), 0.001, 1e-15);
    EXPECT_EQ(daily[0][3], 6);
    EXPECT_EQ(daily[1][0], 20240103);
    EXPECT_DOUBLE_EQ(daily[1][1].get<double>(), 0.001);
    EXPECT_EQ(daily[1][3], 1);
}

TEST_F(FundingStoreBehaviour, R14_MillisecondJitterRoundsToTheClosingMidnight)
{
    MarketDataStore store(path_);
    store.storeFundingRates({{"BTCUSDT", {{JAN3_MS + 123, 0.001, 100}}},
                            {"ETHUSDT", {{JAN3_MS - 100, 0.002, 100}}}});
    const auto daily = rows("SELECT pair,date,events FROM funding_rate_daily ORDER BY pair");
    EXPECT_EQ(daily, Json::array({Json::array({"BTCUSDT", 20240102, 1}),
                                 Json::array({"ETHUSDT", 20240102, 1})}));
    EXPECT_EQ(rows("SELECT funding_time FROM funding_rate_events ORDER BY pair"),
        Json::array({Json::array({1704240000123LL}), Json::array({1704239999900LL})}));
}

TEST_F(FundingStoreBehaviour, R13_InvalidSettlementRollsBackTheEntireFundingBatch)
{
    MarketDataStore store(path_);
    store.storeFundingRates({{"BTCUSDT", {{JAN1_MS, 0.001, 100}}}});
    for (const FundingRateEvent& invalid : {
            FundingRateEvent{0, 0.001, 100}, FundingRateEvent{-1, 0.001, 100},
            FundingRateEvent{JAN2_MS, 1.0, 100}, FundingRateEvent{JAN2_MS, -1.0, 100},
            FundingRateEvent{JAN2_MS, std::numeric_limits<double>::infinity(), 100},
            FundingRateEvent{JAN2_MS, std::nan(""), 100}}) {
        SCOPED_TRACE(invalid.funding_time_ms);
        EXPECT_THROW(store.storeFundingRates({{"BTCUSDT", {{JAN2_MS, 0.002, 100}}},
                                              {"ETHUSDT", {invalid}}}), std::exception);
        EXPECT_EQ(rows("SELECT pair,funding_time,rate FROM funding_rate_events"),
            Json::array({Json::array({"BTCUSDT", JAN1_MS, 0.001})}));
    }
}

TEST_F(FundingStoreBehaviour, R15_InitialPlanIncludesHistoricalPairsAndStartsAtFirstBar)
{
    auto bars = barsFor("BTCUSDT", {20240101, 20240102});
    bars.data["OLDUSDT"][20240102] = exampleBar();
    seedBars(bars);
    MarketDataStore store(path_);
    store.storeFundingRates({{"NOBARSUSDT", {{JAN1_MS, 0.001, 100}}}});
    const auto plan = store.buildFundingPlan();
    ASSERT_EQ(plan.size(), 2U);
    EXPECT_EQ(plan[0].symbol, "BTCUSDT");
    EXPECT_EQ(plan[0].start_ms, 1704067200000LL);
    EXPECT_EQ(plan[1].symbol, "OLDUSDT");
    EXPECT_EQ(plan[1].start_ms, 1704153600000LL);
}

TEST_F(FundingStoreBehaviour, R15_ResumeStartsOneMillisecondAfterLatestPersistedSettlement)
{
    seedBars(barsFor("BTCUSDT", {20240101, 20240102}));
    MarketDataStore store(path_);
    store.storeFundingRates({{"BTCUSDT", {{JAN2_MS, 0.001, 100},
                                         {JAN2_MS + 8 * HOUR_MS, -0.002, 100}}}});
    const auto plan = store.buildFundingPlan();
    ASSERT_EQ(plan.size(), 1U);
    EXPECT_EQ(plan.front().symbol, "BTCUSDT");
    EXPECT_EQ(plan.front().start_ms, 1704182400001LL);
}

TEST_F(FundingStoreBehaviour, R15_ClosingMidnightCompletionToleranceHasAnInclusiveBoundary)
{
    seedBars(barsFor("BTCUSDT", {20240102}));
    MarketDataStore store(path_);
    store.storeFundingRates({{"BTCUSDT", {{JAN3_MS - 60001, 0.001, 100}}}});
    auto plan = store.buildFundingPlan();
    ASSERT_EQ(plan.size(), 1U);
    EXPECT_EQ(plan.front().start_ms, JAN3_MS - 60000);
    store.storeFundingRates({{"BTCUSDT", {{JAN3_MS - 60000, 0.001, 100}}}});
    EXPECT_TRUE(store.buildFundingPlan().empty());
}

} // namespace
} // namespace market_data_test
