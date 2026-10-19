#include <memory>
#include <stdexcept>
#include <string>

#include <libpq-fe.h>

#include "postgres_state_store.h"
#include "state_store_contract.h"
#include "test_support.h"

namespace persistence_test {

class PostgresBackend : public testing::Test {
protected:
    inline static std::unique_ptr<test_support::DockerContainer> database_;
    std::unique_ptr<StateStore> store_;

    void SetUp() override
    {
        if (!test_support::dockerAvailable()) GTEST_SKIP() << "Docker is not available";
        if (!database_)
            database_ = test_support::DockerContainer::startPostgres();
        // Each case owns an empty schema in this executable's throwaway container.
        executeSql("DROP SCHEMA public CASCADE; CREATE SCHEMA public;");
        reopen();
    }

    static void TearDownTestSuite() { database_.reset(); }

    void reopen()
    {
        store_.reset();
        store_ = std::make_unique<PostgresStateStore>(database_->url());
    }

    void executeSql(const char* sql)
    {
        const std::unique_ptr<PGconn, decltype(&PQfinish)> connection(
            PQconnectdb(database_->url().c_str()), PQfinish);
        if (!connection || PQstatus(connection.get()) != CONNECTION_OK)
            throw std::runtime_error("Cannot connect to isolated PostgreSQL test database");
        const std::unique_ptr<PGresult, decltype(&PQclear)> result(
            PQexec(connection.get(), sql), PQclear);
        if (!result || PQresultStatus(result.get()) != PGRES_COMMAND_OK)
            throw std::runtime_error("Isolated PostgreSQL test SQL failed");
    }

    void failSnapshotWrites(bool enabled)
    {
        executeSql(enabled
            ? "CREATE FUNCTION reject_snapshot() RETURNS trigger LANGUAGE plpgsql AS $$ "
              "BEGIN RAISE EXCEPTION 'injected snapshot failure'; END; $$; "
              "CREATE TRIGGER reject_snapshot BEFORE INSERT OR UPDATE ON trading_runtime_state "
              "FOR EACH ROW EXECUTE FUNCTION reject_snapshot();"
            : "DROP TRIGGER reject_snapshot ON trading_runtime_state; DROP FUNCTION reject_snapshot();");
    }
};

using PostgresTypes = testing::Types<PostgresBackend>;
INSTANTIATE_TYPED_TEST_SUITE_P(Postgres, StateStoreContract, PostgresTypes);

TEST_F(PostgresBackend, R17_CommittedStateAndAuditSurviveDatabaseRestart)
{
    if (!test_support::dockerAvailable()) GTEST_SKIP() << "Docker is not available";
    store_->save(exampleSnapshot(), exampleFill());
    store_.reset(); // Reconnect after restart; no assumption of transparent client reconnect.
    database_->restart();
    reopen();
    const auto loaded = store_->load();
    ASSERT_TRUE(loaded.has_value());
    expectSnapshot(*loaded, exampleSnapshot());
    const auto fills = store_->loadFills();
    ASSERT_EQ(fills.size(), 1U);
    expectFill(fills.front(), exampleFill());
}

TEST_F(PostgresBackend, R17_IndependentReaderSeesCommittedReplacement)
{
    if (!test_support::dockerAvailable()) GTEST_SKIP() << "Docker is not available";
    store_->save(exampleSnapshot(), exampleFill());
    PostgresStateStore reader(database_->url());
    TradingStateSnapshot flat;
    flat.account_cash = 1048.0;
    flat.next_order_id = 43;
    store_->save(flat);
    const auto loaded = reader.load();
    ASSERT_TRUE(loaded.has_value());
    expectSnapshot(*loaded, flat);
    const auto fills = reader.loadFills();
    ASSERT_EQ(fills.size(), 1U);
    expectFill(fills.front(), exampleFill());
}

} // namespace persistence_test
