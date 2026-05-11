#include "backtest.h"

#include <stdexcept>


/**************************************************************************************
 * Purpose : Persist all trades to SQLite
 **************************************************************************************/
void Backtester::storeTrades(sqlite3* db)
{
    const char* createTableSQL = R"(
        CREATE TABLE IF NOT EXISTS trades (
            trade_id       INTEGER PRIMARY KEY,
            start_ts       INTEGER NOT NULL,
            end_ts         INTEGER NOT NULL,
            commission     REAL NOT NULL,
            coin           TEXT NOT NULL,
            direction      INTEGER NOT NULL,
            current_price  REAL NOT NULL,
            entry_price    REAL NOT NULL,
            exit_price     REAL NOT NULL,
            size           REAL NOT NULL,
            is_simulated   INTEGER NOT NULL,
            exited         INTEGER NOT NULL,
            pnl            REAL NOT NULL
        );
    )";

    if (sqlite3_exec(db, createTableSQL, nullptr, nullptr, nullptr) != SQLITE_OK) {
        throw std::runtime_error("Failed to create trades table");
    }

    const char* createIndexSQL = R"(
        CREATE INDEX IF NOT EXISTS idx_trades_coin
            ON trades(coin);

        CREATE INDEX IF NOT EXISTS idx_trades_start_ts
            ON trades(start_ts);
    )";

    if (sqlite3_exec(db, createIndexSQL, nullptr, nullptr, nullptr) != SQLITE_OK) {
        throw std::runtime_error("Failed to create trades indexes");
    }

    const char* insertSQL = R"(
        INSERT INTO trades (
            trade_id, start_ts, end_ts, commission, coin, direction,
            current_price, entry_price, exit_price, size,
            is_simulated, exited, pnl
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(trade_id) DO UPDATE SET
            start_ts      = excluded.start_ts,
            end_ts        = excluded.end_ts,
            commission    = excluded.commission,
            coin          = excluded.coin,
            direction     = excluded.direction,
            current_price = excluded.current_price,
            entry_price   = excluded.entry_price,
            exit_price    = excluded.exit_price,
            size          = excluded.size,
            is_simulated  = excluded.is_simulated,
            exited        = excluded.exited,
            pnl           = excluded.pnl;
    )";

    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db, insertSQL, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare trades insert statement");
    }

    try {
        auto& allTrades = backtest_context_.GetTradesHistory();

        for (auto& strategyInstance : backtest_context_.GetStrategyPortfolio()) {
            for (auto& trade : strategyInstance.GetCurrentTrades()) {
                allTrades[trade.trade_id_] = trade;
            }
        }

        if (sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr) != SQLITE_OK) {
            throw std::runtime_error("Failed to begin transaction for trades");
        }

        for (const auto& [tradeId, trade] : allTrades) {
            (void)tradeId;

            sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(trade.trade_id_));
            sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(trade.start_));
            sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(trade.end_));
            sqlite3_bind_double(stmt, 4, trade.commission_);
            sqlite3_bind_text(stmt, 5, trade.coin_.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 6, static_cast<int>(trade.direction_));
            sqlite3_bind_double(stmt, 7, trade.current_price_);
            sqlite3_bind_double(stmt, 8, trade.entry_);
            sqlite3_bind_double(stmt, 9, trade.exit_);
            sqlite3_bind_double(stmt, 10, trade.size_);
            sqlite3_bind_int(stmt, 11, trade.isSimulated_ ? 1 : 0);
            sqlite3_bind_int(stmt, 12, trade.exited_ ? 1 : 0);
            sqlite3_bind_double(stmt, 13, trade.pnl_);

            if (sqlite3_step(stmt) != SQLITE_DONE) {
                throw std::runtime_error("Failed to insert trade row");
            }

            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
        }

        if (sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
            throw std::runtime_error("Failed to commit transaction for trades");
        }
    }
    catch (...) {
        sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
        sqlite3_finalize(stmt);
        throw;
    }

    sqlite3_finalize(stmt);
}


/**************************************************************************************
 * Purpose : Persist balance/equity history to SQLite
 **************************************************************************************/
void Backtester::storeBalanceEquity(sqlite3* db)
{
    const char* createTableSQL = R"(
        CREATE TABLE IF NOT EXISTS balance_equity (
            ts      INTEGER PRIMARY KEY AUTOINCREMENT,
            balance REAL NOT NULL,
            equity  REAL NOT NULL
        );
    )";

    if (sqlite3_exec(db, createTableSQL, nullptr, nullptr, nullptr) != SQLITE_OK) {
        throw std::runtime_error("Failed to create balance_equity table");
    }

    const char* insertWithTsSQL =
        "INSERT INTO balance_equity (ts, balance, equity) VALUES (?, ?, ?);";

    const char* insertSQL =
        "INSERT INTO balance_equity (balance, equity) VALUES (?, ?);";

    sqlite3_stmt* stmtWithTs = nullptr;
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db, insertWithTsSQL, -1, &stmtWithTs, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(db, insertSQL, -1, &stmt, nullptr) != SQLITE_OK)
    {
        if (stmtWithTs) {
            sqlite3_finalize(stmtWithTs);
        }

        if (stmt) {
            sqlite3_finalize(stmt);
        }

        throw std::runtime_error("Failed to prepare balance_equity insert statements");
    }

    const auto& history = backtest_context_.GetBalanceEquityHistoric();

    bool first = true;
    sqlite3_int64 ts = static_cast<sqlite3_int64>(starting_date_);

    if (sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_finalize(stmtWithTs);
        sqlite3_finalize(stmt);
        throw std::runtime_error("Failed to begin transaction for balance_equity");
    }

    try {
        for (const auto& [balance, equity] : history) {
            if (first) {
                sqlite3_bind_int64(stmtWithTs, 1, ts);
                sqlite3_bind_double(stmtWithTs, 2, balance);
                sqlite3_bind_double(stmtWithTs, 3, equity);

                if (sqlite3_step(stmtWithTs) != SQLITE_DONE) {
                    throw std::runtime_error("Failed to insert first balance_equity row");
                }

                sqlite3_reset(stmtWithTs);
                sqlite3_clear_bindings(stmtWithTs);

                first = false;
            } else {
                sqlite3_bind_double(stmt, 1, balance);
                sqlite3_bind_double(stmt, 2, equity);

                if (sqlite3_step(stmt) != SQLITE_DONE) {
                    throw std::runtime_error("Failed to insert balance_equity row");
                }

                sqlite3_reset(stmt);
                sqlite3_clear_bindings(stmt);
            }
        }

        if (sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
            throw std::runtime_error("Failed to commit transaction for balance_equity");
        }
    }
    catch (...) {
        sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
        sqlite3_finalize(stmtWithTs);
        sqlite3_finalize(stmt);
        throw;
    }

    sqlite3_finalize(stmtWithTs);
    sqlite3_finalize(stmt);
}


/**************************************************************************************
 * Purpose : Persist all backtest results to disk
 **************************************************************************************/
void Backtester::storeResults(std::filesystem::path& backtest_store_path)
{
    std::filesystem::create_directories(backtest_store_path.parent_path());

    sqlite3* db = nullptr;

    if (sqlite3_open(backtest_store_path.c_str(), &db) != SQLITE_OK) {
        throw std::runtime_error("Failed to create SQLite database");
    }

    try {
        storeBalanceEquity(db);
        storeTrades(db);
    }
    catch (...) {
        sqlite3_close(db);
        throw;
    }

    sqlite3_close(db);
}


/**************************************************************************************
 * Purpose : Copy all currently open trades into trade history
 **************************************************************************************/
void Backtester::closeTrades()
{
    auto& allTrades = backtest_context_.GetTradesHistory();

    for (auto& strategyInstance : backtest_context_.GetStrategyPortfolio()) {
        for (auto& trade : strategyInstance.GetCurrentTrades()) {
            allTrades[trade.trade_id_] = trade;
        }
    }
}