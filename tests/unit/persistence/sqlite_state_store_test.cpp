#include "sqlite_backend.h"
#include "state_store_contract.h"

namespace persistence_test {

using SQLiteTypes = testing::Types<SQLiteBackend>;
INSTANTIATE_TYPED_TEST_SUITE_P(SQLite, StateStoreContract, SQLiteTypes);

} // namespace persistence_test
