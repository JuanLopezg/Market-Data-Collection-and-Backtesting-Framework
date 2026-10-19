#pragma once

#include <memory>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include "sqlite_state_store.h"
#include "test_support.h"

namespace persistence_test {

class SQLiteBackend : public testing::Test {
protected:
    test_support::TempDir directory_{"persistence-sqlite"};
    std::unique_ptr<StateStore> store_;

    void SetUp() override { reopen(); }

    void reopen()
    {
        store_.reset();
        store_ = std::make_unique<SQLiteStateStore>((directory_ / "state.sqlite").string());
    }

    void executeSql(const char* sql)
    {
        sqlite3* raw = nullptr;
        const int opened = sqlite3_open((directory_ / "state.sqlite").c_str(), &raw);
        const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
        if (opened != SQLITE_OK)
            throw std::runtime_error("Cannot open isolated SQLite test database");
        if (sqlite3_exec(db.get(), sql, nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db.get()));
    }

    void failSnapshotWrites(bool enabled)
    {
        executeSql(enabled
            ? "CREATE TRIGGER reject_snapshot BEFORE INSERT ON runtime_state "
              "BEGIN SELECT RAISE(ABORT, 'injected snapshot failure'); END;"
            : "DROP TRIGGER reject_snapshot;");
    }
};

} // namespace persistence_test
