#include "sqlite_backend.h"
#include "state_store_contract.h"

namespace persistence_test {

using SQLiteTypes = testing::Types<SQLiteBackend>;
INSTANTIATE_TYPED_TEST_SUITE_P(SQLite, StateStoreContract, SQLiteTypes);

// R6 and R8 are checked per backend, next to their PostgreSQL equivalents.
TEST_F(SQLiteBackend, R6_LoadRejectsUnknownSchema)
{
    auto snapshot = exampleSnapshot();
    snapshot.schema_version = TradingStateSnapshot::CURRENT_SCHEMA_VERSION + 1;
    store_->save(snapshot);
    reopen();
    EXPECT_THROW((void)store_->load(), std::exception);
}

TEST_F(SQLiteBackend, R8_ManualReferencePriceAndImmediateFillIntentSurvive)
{
    auto snapshot = exampleSnapshot();
    auto& order = snapshot.orders.front();
    order.status = ExecutionOrderStatus::Created;
    order.filled_quantity = 0.0;
    order.request.quantity = 2.0;
    order.request.reference_price = 75.0;
    order.request.fill_at_reference_price = true;
    store_->save(snapshot);
    reopen();
    const auto loaded = store_->load();
    ASSERT_TRUE(loaded.has_value());
    expectSnapshot(*loaded, snapshot);
}

TEST_F(SQLiteBackend, R8_LivePositionStopRemainsConditionalAfterRestart)
{
    auto snapshot = exampleSnapshot();
    auto& order = snapshot.orders.front();
    order.request.side = OrderSide::Sell;
    order.request.quantity = 2.0;
    order.request.position_stop_price = 60.0;
    order.status = ExecutionOrderStatus::Created;
    order.filled_quantity = 0.0;
    store_->save(snapshot);
    reopen();
    const auto loaded = store_->load();
    ASSERT_TRUE(loaded.has_value());
    expectSnapshot(*loaded, snapshot);
}

} // namespace persistence_test
