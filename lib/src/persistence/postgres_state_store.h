#pragma once

#include <memory>
#include <string>

#include "state_store.h"


/**************************************************************************************
 * Type    : PostgresStateStore
 * Purpose : Distributed-runtime StateStore backed by PostgreSQL
 *
 * The operational snapshot is stored as one JSONB document and the fill audit trail is
 * append-only. save(snapshot, fill) commits both inside one PostgreSQL transaction.
 **************************************************************************************/
class PostgresStateStore final : public StateStore {
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

public:
    explicit PostgresStateStore(const std::string& connectionString);
    ~PostgresStateStore() override;

    PostgresStateStore(const PostgresStateStore&) = delete;
    PostgresStateStore& operator=(const PostgresStateStore&) = delete;

    void save(
        const TradingStateSnapshot& snapshot,
        const std::optional<Fill>& newFill = std::nullopt
    ) override;

    std::optional<TradingStateSnapshot> load() const override;
    std::vector<Fill> loadFills() const override;
};
