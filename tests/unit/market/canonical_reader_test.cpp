#include <gtest/gtest.h>

#include "canonical_market_data_reader.h"
#include "test_support.h"

#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

#include <sqlite3.h>

namespace {

class CanonicalMarket : public ::testing::Test {
protected:
    test_support::TempDir directory;
    const std::filesystem::path path = directory / "market.sqlite";
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> writer{nullptr, sqlite3_close};

    void execute(const std::string& sql)
    {
        char* error = nullptr;
        const int result = sqlite3_exec(writer.get(), sql.c_str(), nullptr, nullptr, &error);
        const std::string message = error ? error : "";
        sqlite3_free(error);
        if (result != SQLITE_OK) throw std::runtime_error("Fixture SQL failed: " + message);
    }

    void insertBar(const Coin& coin, Timestamp date, const OHLCV& bar)
    {
        sqlite3_stmt* raw = nullptr;
        const int prepared = sqlite3_prepare_v2(writer.get(),
            "INSERT INTO ohlcv_data(pair,date,open,high,low,close,volume,quote_volume) "
            "VALUES(?,?,?,?,?,?,?,?)", -1, &raw, nullptr);
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement{raw, sqlite3_finalize};
        if (prepared != SQLITE_OK) throw std::runtime_error("Cannot prepare fixture bar");
        if (sqlite3_bind_text(raw, 1, coin.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_int64(raw, 2, date) != SQLITE_OK ||
            sqlite3_bind_double(raw, 3, bar.open) != SQLITE_OK ||
            sqlite3_bind_double(raw, 4, bar.high) != SQLITE_OK ||
            sqlite3_bind_double(raw, 5, bar.low) != SQLITE_OK ||
            sqlite3_bind_double(raw, 6, bar.close) != SQLITE_OK ||
            sqlite3_bind_double(raw, 7, bar.volume) != SQLITE_OK)
            throw std::runtime_error("Cannot bind fixture bar");
        const int quoteBound = std::isnan(bar.quote_volume)
            ? sqlite3_bind_null(raw, 8) : sqlite3_bind_double(raw, 8, bar.quote_volume);
        if (quoteBound != SQLITE_OK || sqlite3_step(raw) != SQLITE_DONE)
            throw std::runtime_error("Cannot insert fixture bar");
    }

    void SetUp() override
    {
        sqlite3* raw = nullptr;
        const int opened = sqlite3_open(path.string().c_str(), &raw);
        writer.reset(raw);
        ASSERT_EQ(opened, SQLITE_OK);
        execute(
            "CREATE TABLE ohlcv_data ("
            "pair TEXT NOT NULL,date INTEGER NOT NULL,open REAL NOT NULL,high REAL NOT NULL,"
            "low REAL NOT NULL,close REAL,volume REAL NOT NULL,quote_volume REAL,"
            "PRIMARY KEY(pair,date));"
            "CREATE TABLE market_volume_rank_daily ("
            "date INTEGER NOT NULL,rank INTEGER NOT NULL,pair TEXT NOT NULL,"
            "quote_volume REAL NOT NULL,PRIMARY KEY(date,rank),UNIQUE(date,pair));"
            "INSERT INTO market_volume_rank_daily VALUES"
            "(20240229,1,'ETH',2000),(20240229,2,'BTC',1000),"
            "(20240301,1,'BTC',3000),(20240301,2,'ETH',2000),(20240301,3,'OTHER',1000),"
            "(20240302,1,'OTHER',900000),(20240302,2,'ETH',2000);"
        );
        // Five calendar days straddle the three-day decision window and leap day.
        insertBar("BTC", 20240227, {80, 100, 70, 90, 3, 261});
        insertBar("BTC", 20240228, {90, 110, 80, 100, 3, 291});
        insertBar("BTC", 20240229, {100, 115, 90, 105, 3, 306});
        insertBar("BTC", 20240301, {100, 120, 90, 110, 3, 321});
        insertBar("BTC", 20240302, {9000, 10010, 8990, 10000, 3, 30000});
        insertBar("ETH", 20240227, {17, 20, 15, 18, 5, 81});
        insertBar("ETH", 20240228, {18, 21, 16, 19, 5, 86});
        insertBar("ETH", 20240229, {20, 23, 18, 21, 5, 96});
        insertBar("ETH", 20240301, {20, 24, 18, 22, 5, 101});
        insertBar("ETH", 20240302, {1900, 2002, 1898, 2000, 5, 10000});
        for (Timestamp date : {20240227u, 20240228u, 20240229u, 20240301u, 20240302u}) {
            insertBar("HELD", date, {10, 12, 9, 11, 7, 71});
            insertBar("OTHER", date, {5, 6, 4, 5.5, 8, 41});
        }
    }
};

TEST_F(CanonicalMarket, R9_TopNRestrictsCurrentEntriesAndLoadedHistory)
{
    CanonicalMarketDataReader reader(path);
    const auto window = reader.loadWindow(20240301, 3, 2);
    EXPECT_EQ(window.ranked_symbols, 2u);
    EXPECT_EQ(window.history_symbols, 2u);
    EXPECT_EQ(window.history_rows, 6u);
    ASSERT_EQ(window.market_data.size(), 1u);
    ASSERT_EQ(window.market_data.at(20240301).size(), 2u);
    EXPECT_DOUBLE_EQ(window.market_data.at(20240301).at("BTC").close, 110);
    EXPECT_DOUBLE_EQ(window.market_data.at(20240301).at("ETH").close, 22);
    EXPECT_FALSE(window.raw_data.data.contains("HELD"));
    EXPECT_FALSE(window.raw_data.data.contains("OTHER"));
}

TEST_F(CanonicalMarket, R9_HeldSymbolsSupplyExitHistoryWithoutBecomingEntryCandidates)
{
    CanonicalMarketDataReader reader(path);
    const auto window = reader.loadWindow(20240301, 3, 2, {"HELD"});
    EXPECT_EQ(window.ranked_symbols, 2u);
    EXPECT_EQ(window.history_symbols, 3u);
    EXPECT_EQ(window.history_rows, 9u);
    ASSERT_EQ(window.raw_data.data.at("HELD").size(), 3u);
    EXPECT_DOUBLE_EQ(window.raw_data.data.at("HELD").at(20240301).close, 11);
    EXPECT_FALSE(window.market_data.at(20240301).contains("HELD"));
    EXPECT_FALSE(window.raw_data.data.contains("OTHER"));
}

TEST_F(CanonicalMarket, R9_AlreadyRankedExtraHistoryCannotDuplicateRows)
{
    CanonicalMarketDataReader reader(path);
    const auto window = reader.loadWindow(20240301, 3, 2, {"BTC"});
    EXPECT_EQ(window.history_rows, 6u);
    EXPECT_EQ(window.history_symbols, 2u);
    EXPECT_EQ(window.market_data.at(20240301).size(), 2u);
}

TEST_F(CanonicalMarket, R9_TopOneUsesTheTargetDaysPersistedRank)
{
    CanonicalMarketDataReader reader(path);
    const auto window = reader.loadWindow(20240301, 3, 1);
    ASSERT_EQ(window.market_data.at(20240301).size(), 1u);
    EXPECT_TRUE(window.market_data.at(20240301).contains("BTC"));
    EXPECT_FALSE(window.raw_data.data.contains("ETH"));
    EXPECT_FALSE(window.raw_data.data.contains("OTHER"));
    EXPECT_EQ(window.history_rows, 3u);
}

TEST_F(CanonicalMarket, R10_InclusiveCalendarWindowCrossesLeapDayAndExcludesFuture)
{
    CanonicalMarketDataReader reader(path);
    const auto window = reader.loadWindow(20240301, 3, 1);
    const auto& history = window.raw_data.data.at("BTC");
    ASSERT_EQ(history.size(), 3u);
    EXPECT_DOUBLE_EQ(history.at(20240228).close, 100);
    EXPECT_DOUBLE_EQ(history.at(20240229).close, 105);
    EXPECT_DOUBLE_EQ(history.at(20240301).close, 110);
    EXPECT_FALSE(history.contains(20240227));
    EXPECT_FALSE(history.contains(20240302));
    EXPECT_EQ(window.market_data.at(20240301).at("BTC").barNumber, 3u);
}

TEST_F(CanonicalMarket, R10_OneDayWindowContainsOnlyTheCompletedTargetDay)
{
    CanonicalMarketDataReader reader(path);
    const auto window = reader.loadWindow(20240301, 1, 1);
    ASSERT_EQ(window.raw_data.data.at("BTC").size(), 1u);
    EXPECT_DOUBLE_EQ(window.raw_data.data.at("BTC").at(20240301).close, 110);
    EXPECT_EQ(window.market_data.at(20240301).at("BTC").barNumber, 1u);
}

TEST_F(CanonicalMarket, R10_AddingAnExtremeFutureBarCannotChangeAnEarlierDecisionWindow)
{
    CanonicalMarketDataReader reader(path);
    const auto before = reader.loadWindow(20240301, 3, 1);
    insertBar("BTC", 20240303, {900000, 1000010, 899990, 1000000, 3, 3000000});
    execute("INSERT INTO market_volume_rank_daily VALUES(20240303,1,'OTHER',9999999)");
    const auto after = reader.loadWindow(20240301, 3, 1);
    EXPECT_EQ(after.history_rows, before.history_rows);
    ASSERT_EQ(after.raw_data.data.at("BTC").size(), 3u);
    for (Timestamp date : {20240228u, 20240229u, 20240301u})
        EXPECT_DOUBLE_EQ(after.raw_data.data.at("BTC").at(date).close,
                         before.raw_data.data.at("BTC").at(date).close);
    EXPECT_FALSE(after.raw_data.data.contains("OTHER"));
    EXPECT_DOUBLE_EQ(after.market_data.at(20240301).at("BTC").close, 110);
}

TEST_F(CanonicalMarket, R3_MissingWarmupDaysDoNotCreateBarsOrAdvanceBarNumbers)
{
    execute("DELETE FROM ohlcv_data WHERE pair='BTC' AND date=20240229");
    CanonicalMarketDataReader reader(path);
    const auto window = reader.loadWindow(20240301, 3, 1);
    ASSERT_EQ(window.raw_data.data.at("BTC").size(), 2u);
    EXPECT_FALSE(window.raw_data.data.at("BTC").contains(20240229));
    EXPECT_EQ(window.market_data.at(20240301).at("BTC").barNumber, 2u);
    EXPECT_EQ(window.history_rows, 2u);
}

TEST_F(CanonicalMarket, R11_RankedAssetWithoutTargetBarCannotUsePreviousOrFutureBar)
{
    execute("DELETE FROM ohlcv_data WHERE pair='BTC' AND date=20240301");
    CanonicalMarketDataReader reader(path);
    EXPECT_ANY_THROW(reader.loadWindow(20240301, 3, 1));
}

TEST_F(CanonicalMarket, R11_HeldAssetWithoutTargetBarCannotSupplyStaleExitHistory)
{
    execute("DELETE FROM ohlcv_data WHERE pair='HELD' AND date=20240301");
    CanonicalMarketDataReader reader(path);
    EXPECT_ANY_THROW(reader.loadWindow(20240301, 3, 2, {"HELD"}));
}

TEST_F(CanonicalMarket, R11_StrategyWindowCannotBorrowAnEarlierOrFutureRanking)
{
    execute("DELETE FROM market_volume_rank_daily WHERE date=20240301");
    CanonicalMarketDataReader reader(path);
    EXPECT_ANY_THROW(reader.loadWindow(20240301, 3, 2));
}

TEST_F(CanonicalMarket, R12_PortfolioHistoryLoadsExplicitUnrankedAssetsWithoutAnyRanking)
{
    execute("DELETE FROM market_volume_rank_daily");
    CanonicalMarketDataReader reader(path);
    const auto history = reader.loadMarketDataWindow(20240301, 3, {"HELD"});
    ASSERT_EQ(history.size(), 3u);
    for (Timestamp date : {20240228u, 20240229u, 20240301u}) {
        ASSERT_EQ(history.at(date).size(), 1u);
        EXPECT_DOUBLE_EQ(history.at(date).at("HELD").close, 11);
    }
    EXPECT_FALSE(history.contains(20240227));
    EXPECT_FALSE(history.contains(20240302));
}

TEST_F(CanonicalMarket, R12_PortfolioHistoryKeepsGapsAndExcludesUnrequestedAssets)
{
    execute("DELETE FROM ohlcv_data WHERE pair='BTC' AND date=20240229");
    CanonicalMarketDataReader reader(path);
    const auto history = reader.loadMarketDataWindow(20240301, 3, {"BTC", "ETH"});
    ASSERT_EQ(history.size(), 3u);
    EXPECT_FALSE(history.at(20240229).contains("BTC"));
    EXPECT_DOUBLE_EQ(history.at(20240229).at("ETH").close, 21);
    EXPECT_DOUBLE_EQ(history.at(20240301).at("BTC").close, 110);
    for (const auto& [date, assets] : history) {
        EXPECT_GE(date, 20240228u);
        EXPECT_LE(date, 20240301u);
        EXPECT_FALSE(assets.contains("HELD"));
        EXPECT_FALSE(assets.contains("OTHER"));
    }
}

TEST_F(CanonicalMarket, R11_PortfolioWindowRejectsAnyRequiredAssetMissingAtTarget)
{
    execute("DELETE FROM ohlcv_data WHERE pair='ETH' AND date=20240301");
    CanonicalMarketDataReader reader(path);
    EXPECT_ANY_THROW(reader.loadMarketDataWindow(20240301, 3, {"BTC", "ETH"}));
    EXPECT_ANY_THROW(reader.loadMarketDataWindow(20240301, 3, {"UNKNOWN"}));
}

TEST_F(CanonicalMarket, R13_ExactClosesUseOnlyRequestedAssetsOnTheCompletedDate)
{
    CanonicalMarketDataReader reader(path);
    const auto prices = reader.loadExactClosingPrices(20240301, {"BTC", "ETH"});
    ASSERT_EQ(prices.size(), 2u);
    EXPECT_DOUBLE_EQ(prices.at("BTC"), 110);
    EXPECT_DOUBLE_EQ(prices.at("ETH"), 22);
    EXPECT_FALSE(prices.contains("HELD"));
    EXPECT_FALSE(prices.contains("OTHER"));
}

TEST_F(CanonicalMarket, R13_EmptyExactCloseRequestNeedsNoMarketObservation)
{
    CanonicalMarketDataReader reader(path);
    EXPECT_TRUE(reader.loadExactClosingPrices(20240101, {}).empty());
}

TEST_F(CanonicalMarket, R13_OneMissingExactCloseFailsTheWholeRequestWithoutFallback)
{
    execute("DELETE FROM ohlcv_data WHERE pair='ETH' AND date=20240301");
    CanonicalMarketDataReader reader(path);
    EXPECT_ANY_THROW(reader.loadExactClosingPrices(20240301, {"BTC", "ETH"}));
    EXPECT_DOUBLE_EQ(reader.loadExactClosingPrices(20240301, {"BTC"}).at("BTC"), 110);
}

TEST_F(CanonicalMarket, R13_InvalidPersistedClosesAreNotValuationReferences)
{
    CanonicalMarketDataReader reader(path);
    for (const char* invalid : {"0", "-1", "NULL", "1e999"}) {
        SCOPED_TRACE(invalid);
        execute(std::string("UPDATE ohlcv_data SET close=") + invalid +
                " WHERE pair='BTC' AND date=20240301");
        EXPECT_ANY_THROW(reader.loadExactClosingPrices(20240301, {"BTC"}));
    }
}

TEST_F(CanonicalMarket, R14_ExactRankingAvailabilityDoesNotFollowOhlcvAvailability)
{
    CanonicalMarketDataReader reader(path);
    EXPECT_FALSE(reader.hasRankingDate(20240228));
    EXPECT_TRUE(reader.hasRankingDate(20240229));
    EXPECT_TRUE(reader.hasRankingDate(20240301));
    EXPECT_FALSE(reader.hasRankingDate(20240303));
}

TEST_F(CanonicalMarket, R14_CatchupFrontierNeverAdvancesBeyondItsRequestedDate)
{
    CanonicalMarketDataReader reader(path);
    EXPECT_FALSE(reader.latestRankingDateAtOrBefore(20240228).has_value());
    EXPECT_EQ(reader.latestRankingDateAtOrBefore(20240229), 20240229u);
    EXPECT_EQ(reader.latestRankingDateAtOrBefore(20240301), 20240301u);
    EXPECT_EQ(reader.latestRankingDateAtOrBefore(20240303), 20240302u);
}

TEST_F(CanonicalMarket, R2_LegacySchemaLeavesQuoteVolumeUnknownWithoutMigration)
{
    execute(
        "DROP TABLE ohlcv_data;"
        "CREATE TABLE ohlcv_data(pair TEXT,date INTEGER,open REAL,high REAL,low REAL,"
        "close REAL,volume REAL,PRIMARY KEY(pair,date));"
        "INSERT INTO ohlcv_data VALUES('BTC',20240301,100,120,90,110,3);"
    );
    const auto before = test_support::readFile(path);
    {
        CanonicalMarketDataReader reader(path);
        const auto strategy = reader.loadWindow(20240301, 1, 1);
        EXPECT_TRUE(std::isnan(strategy.raw_data.data.at("BTC").at(20240301).quote_volume));
        EXPECT_TRUE(std::isnan(strategy.market_data.at(20240301).at("BTC").quote_volume));
        const auto portfolio = reader.loadMarketDataWindow(20240301, 1, {"BTC"});
        EXPECT_TRUE(std::isnan(portfolio.at(20240301).at("BTC").quote_volume));
    }
    EXPECT_EQ(test_support::readFile(path), before);
}

TEST_F(CanonicalMarket, R2_SqlNullQuoteVolumeStaysUnknownAcrossBothWindowPaths)
{
    execute("UPDATE ohlcv_data SET quote_volume=NULL WHERE pair='BTC' AND date=20240301");
    CanonicalMarketDataReader reader(path);
    const auto strategy = reader.loadWindow(20240301, 3, 1);
    EXPECT_TRUE(std::isnan(strategy.raw_data.data.at("BTC").at(20240301).quote_volume));
    EXPECT_TRUE(std::isnan(strategy.market_data.at(20240301).at("BTC").quote_volume));
    const auto portfolio = reader.loadMarketDataWindow(20240301, 3, {"BTC"});
    EXPECT_TRUE(std::isnan(portfolio.at(20240301).at("BTC").quote_volume));
    EXPECT_DOUBLE_EQ(portfolio.at(20240229).at("BTC").quote_volume, 306);
}

TEST_F(CanonicalMarket, R2_ActualAndZeroQuoteTurnoverStaySeparateFromBaseVolume)
{
    execute("UPDATE ohlcv_data SET quote_volume=0 WHERE pair='ETH' AND date=20240301");
    CanonicalMarketDataReader reader(path);
    const auto strategy = reader.loadWindow(20240301, 3, 2);
    EXPECT_DOUBLE_EQ(strategy.raw_data.data.at("BTC").at(20240301).quote_volume, 321);
    EXPECT_DOUBLE_EQ(strategy.market_data.at(20240301).at("BTC").volume, 3);
    EXPECT_DOUBLE_EQ(strategy.market_data.at(20240301).at("BTC").quote_volume, 321);
    EXPECT_DOUBLE_EQ(strategy.market_data.at(20240301).at("ETH").quote_volume, 0);
    const auto portfolio = reader.loadMarketDataWindow(20240301, 3, {"BTC", "ETH"});
    EXPECT_DOUBLE_EQ(portfolio.at(20240301).at("BTC").quote_volume, 321);
    EXPECT_DOUBLE_EQ(portfolio.at(20240301).at("ETH").quote_volume, 0);
}

// FINDING: Both canonical full-bar readers return negative/infinite quote volume
// instead of rejecting corrupt supplied turnover (spec R2).
TEST_F(CanonicalMarket, DISABLED_R2_CorruptSuppliedQuoteTurnoverCannotReachStrategyOrRisk)
{
    CanonicalMarketDataReader reader(path);
    for (const char* invalid : {"-1", "1e999"}) {
        SCOPED_TRACE(invalid);
        execute(std::string("UPDATE ohlcv_data SET quote_volume=") + invalid +
                " WHERE pair='BTC' AND date=20240301");
        EXPECT_ANY_THROW(reader.loadWindow(20240301, 3, 1));
        EXPECT_ANY_THROW(reader.loadMarketDataWindow(20240301, 3, {"BTC"}));
    }
}

// FINDING: The first invalid OHLCV row crashes loadWindow (SIGSEGV, exit 139)
// instead of throwing; both reader error paths access SQLite data after finalizing
// the statement (spec R4). Keep the rejection assertions for future verification.
TEST_F(CanonicalMarket, DISABLED_R4_InvalidOhlcvRowsCannotReachStrategyOrRisk)
{
    CanonicalMarketDataReader reader(path);
    for (const char* invalid : {"high=105", "low=105", "open=0", "volume=-1", "volume=1e999"}) {
        SCOPED_TRACE(invalid);
        execute("UPDATE ohlcv_data SET open=100,high=120,low=90,close=110,volume=3 "
                "WHERE pair='BTC' AND date=20240301");
        execute(std::string("UPDATE ohlcv_data SET ") + invalid +
                " WHERE pair='BTC' AND date=20240301");
        EXPECT_ANY_THROW(reader.loadWindow(20240301, 3, 1));
        EXPECT_ANY_THROW(reader.loadMarketDataWindow(20240301, 3, {"BTC"}));
    }
}

TEST_F(CanonicalMarket, R8_MissingOrEmptyDatabasePathsCannotCreateMarketState)
{
    const auto absent = directory / "missing.sqlite";
    EXPECT_THROW((CanonicalMarketDataReader{absent}), std::runtime_error);
    EXPECT_FALSE(std::filesystem::exists(absent));
    EXPECT_THROW((CanonicalMarketDataReader{std::filesystem::path{}}), std::invalid_argument);
}

TEST_F(CanonicalMarket, R8_MissingCanonicalTablesAreRejected)
{
    execute("DROP TABLE market_volume_rank_daily");
    EXPECT_THROW((CanonicalMarketDataReader{path}), std::runtime_error);
    execute("CREATE TABLE market_volume_rank_daily(date INTEGER,rank INTEGER,pair TEXT,quote_volume REAL);"
            "DROP TABLE ohlcv_data;");
    EXPECT_THROW((CanonicalMarketDataReader{path}), std::runtime_error);
}

TEST_F(CanonicalMarket, R8_InvalidWindowRequestsFailBeforeProducingHistory)
{
    execute("INSERT INTO market_volume_rank_daily VALUES(20230229,1,'BTC',3000)");
    CanonicalMarketDataReader reader(path);
    EXPECT_THROW(reader.loadWindow(0, 3, 1), std::invalid_argument);
    EXPECT_THROW(reader.loadWindow(20240301, 0, 1), std::invalid_argument);
    EXPECT_THROW(reader.loadWindow(20240301, 3, 0), std::invalid_argument);
    EXPECT_THROW(reader.loadWindow(20230229, 3, 1), std::invalid_argument);
    EXPECT_THROW(reader.loadMarketDataWindow(0, 3, {"BTC"}), std::invalid_argument);
    EXPECT_THROW(reader.loadMarketDataWindow(20240301, 0, {"BTC"}), std::invalid_argument);
    EXPECT_THROW(reader.loadMarketDataWindow(20240301, 3, {}), std::invalid_argument);
    EXPECT_THROW(reader.loadMarketDataWindow(20230229, 3, {"BTC"}), std::invalid_argument);
    EXPECT_THROW(reader.loadExactClosingPrices(0, {"BTC"}), std::invalid_argument);
    EXPECT_THROW(reader.latestRankingDateAtOrBefore(0), std::invalid_argument);
}

TEST_F(CanonicalMarket, R8_ReopeningReadOnlyReaderPreservesDatabaseAndDecisionInputs)
{
    const auto before = test_support::readFile(path);
    {
        CanonicalMarketDataReader reader(path);
        const auto window = reader.loadWindow(20240301, 3, 2, {"HELD"});
        EXPECT_EQ(window.history_rows, 9u);
        EXPECT_DOUBLE_EQ(window.market_data.at(20240301).at("BTC").close, 110);
        EXPECT_EQ(reader.latestRankingDateAtOrBefore(20240301), 20240301u);
    }
    {
        CanonicalMarketDataReader restarted(path);
        const auto window = restarted.loadWindow(20240301, 3, 2, {"HELD"});
        EXPECT_EQ(window.history_rows, 9u);
        EXPECT_DOUBLE_EQ(window.market_data.at(20240301).at("BTC").close, 110);
        EXPECT_DOUBLE_EQ(restarted.loadExactClosingPrices(20240301, {"BTC"}).at("BTC"), 110);
        EXPECT_EQ(restarted.latestRankingDateAtOrBefore(20240301), 20240301u);
    }
    EXPECT_EQ(test_support::readFile(path), before);
}

} // namespace
