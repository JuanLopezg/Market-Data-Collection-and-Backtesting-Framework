#include <string>

#include <gtest/gtest.h>

#include "config.h"
#include "market_data_examples.h"
#include "test_support.h"

namespace market_data_test {
namespace {

class MarketDataConfigBehaviour : public testing::Test {
protected:
    test_support::TempDir temp_;

    MarketDataConfig load(const Json& document)
    {
        test_support::writeFile(temp_ / "config.json", document.dump());
        return loadMarketDataConfig(temp_ / "config.json");
    }
};

TEST_F(MarketDataConfigBehaviour, R1_DefaultProfileKeepsExecutionPricesOptIn)
{
    const auto config = load(Json::object());
    EXPECT_EQ(config.main_exchange, "binance");
    EXPECT_EQ(config.ranking_size, 100U);
    EXPECT_EQ(config.active_top_n, 50U);
    EXPECT_EQ(config.minimum_history_days, 100);
    EXPECT_EQ(config.retain_after_top_n_days, 365);
    EXPECT_EQ(config.max_parallel_requests, 64U);
    EXPECT_EQ(config.retry_after_failure_seconds, 300);
    EXPECT_EQ(config.midnight_delay_ms, 0);
    EXPECT_TRUE(config.download_funding_rates);
    EXPECT_FALSE(config.publish_paper_execution_prices);
}

TEST_F(MarketDataConfigBehaviour, R1_ValidProfilePreservesUnitsAndExplicitOptIns)
{
    const auto config = load(Json{
        {"database_path", (temp_ / "market.db").string()},
        {"binance_base_url", "http://127.0.0.1:1234/"},
        {"nats_url", "nats://127.0.0.1:4321"}, {"stream", "MARKET_TEST"},
        {"ranking_size", 2}, {"active_top_n", 1}, {"minimum_history_days", 3},
        {"retain_after_top_n_days", 3}, {"max_parallel_requests", 2},
        {"retry_after_failure_seconds", 1}, {"midnight_delay_ms", 0},
        {"publish_paper_execution_prices", true}, {"download_funding_rates", false}});
    EXPECT_EQ(config.database_path, temp_ / "market.db");
    EXPECT_EQ(config.binance_base_url, "http://127.0.0.1:1234");
    EXPECT_EQ(config.nats_url, "nats://127.0.0.1:4321");
    EXPECT_EQ(config.stream, "MARKET_TEST");
    EXPECT_EQ(config.ranking_size, 2U);
    EXPECT_EQ(config.active_top_n, 1U);
    EXPECT_EQ(config.minimum_history_days, 3);
    EXPECT_EQ(config.retain_after_top_n_days, 3);
    EXPECT_EQ(config.max_parallel_requests, 2U);
    EXPECT_EQ(config.retry_after_failure_seconds, 1);
    EXPECT_EQ(config.midnight_delay_ms, 0);
    EXPECT_TRUE(config.publish_paper_execution_prices);
    EXPECT_FALSE(config.download_funding_rates);
}

TEST_F(MarketDataConfigBehaviour, R1_RejectsUnsupportedVenueAndEmptyDestinations)
{
    EXPECT_THROW((void)load(Json{{"main_exchange", "kraken"}}), std::exception);
    for (const std::string key : {"database_path", "binance_base_url", "nats_url", "stream"}) {
        SCOPED_TRACE(key);
        EXPECT_THROW((void)load(Json{{key, ""}}), std::exception);
    }
}

TEST_F(MarketDataConfigBehaviour, R1_RejectsNonpositiveOperationalLimits)
{
    for (const std::string key : {"ranking_size", "active_top_n", "retain_after_top_n_days",
            "minimum_history_days", "max_parallel_requests", "retry_after_failure_seconds"}) {
        for (int value : {0, -1}) {
            SCOPED_TRACE(key + "=" + std::to_string(value));
            EXPECT_THROW((void)load(Json{{key, value}}), std::exception);
        }
    }
}

TEST_F(MarketDataConfigBehaviour, R1_EnforcesRankingHistoryAndMillisecondDelayBounds)
{
    for (const Json& bad : {
            Json{{"ranking_size", 2}, {"active_top_n", 3}},
            Json{{"ranking_size", 1001}}, Json{{"minimum_history_days", 1501}},
            Json{{"midnight_delay_ms", -1}}, Json{{"midnight_delay_ms", 300001}},
            Json{{"midnight_delay_seconds", 1}}}) {
        SCOPED_TRACE(bad.dump());
        EXPECT_THROW((void)load(bad), std::exception);
    }
    EXPECT_NO_THROW((void)load(Json{{"ranking_size", 1000}, {"minimum_history_days", 1500},
                                  {"midnight_delay_ms", 300000}}));
}

TEST_F(MarketDataConfigBehaviour, R1_RejectsUnreadableMalformedOrMistypedConfiguration)
{
    EXPECT_THROW((void)loadMarketDataConfig(temp_ / "absent.json"), std::exception);
    test_support::writeFile(temp_ / "config.json", "{broken");
    EXPECT_THROW((void)loadMarketDataConfig(temp_ / "config.json"), std::exception);
    for (const Json& bad : {Json{{"ranking_size", "two"}}, Json{{"database_path", 5}},
            Json{{"download_funding_rates", "false"}}, Json{{"publish_paper_execution_prices", 1}}}) {
        SCOPED_TRACE(bad.dump());
        EXPECT_THROW((void)load(bad), std::exception);
    }
}

TEST_F(MarketDataConfigBehaviour, R1_RejectsNonObjectDocumentsRatherThanStartingWithDefaults)
{
    for (const Json& bad : {Json::array(), Json(nullptr), Json(5), Json("binance")}) {
        SCOPED_TRACE(bad.dump());
        EXPECT_THROW((void)load(bad), std::exception);
    }
}

} // namespace
} // namespace market_data_test
