#pragma once

#include <string>

#include <sqlite3.h>

#include "state_store.h"


/**************************************************************************************
 * Type    : SQLiteStateStore
 * Purpose : Small inspectable persistence implementation for one trading engine
 **************************************************************************************/
class SQLiteStateStore final : public StateStore {
private:
    sqlite3* db_ = nullptr;

    void createSchema();
    void exec(const char* sql) const;

public:
    explicit SQLiteStateStore(const std::string& path);
    ~SQLiteStateStore() override;

    SQLiteStateStore(const SQLiteStateStore&) = delete;
    SQLiteStateStore& operator=(const SQLiteStateStore&) = delete;

    void save(
        const TradingStateSnapshot& snapshot,
        const std::optional<Fill>& newFill = std::nullopt
    ) override;

    std::optional<TradingStateSnapshot> load() const override;
    std::vector<Fill> loadFills() const override;
};
