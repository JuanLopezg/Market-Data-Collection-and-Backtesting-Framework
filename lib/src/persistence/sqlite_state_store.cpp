#include "sqlite_state_store.h"

#include <stdexcept>
#include <utility>


namespace {

void checkSqlite(int rc, sqlite3* db, const char* context)
{
    if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW)
        throw std::runtime_error(std::string(context) + ": " + sqlite3_errmsg(db));
}

void bindText(sqlite3_stmt* stmt, int index, const std::string& value)
{
    checkSqlite(sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT),
                sqlite3_db_handle(stmt), "sqlite bind text");
}

sqlite3_stmt* prepare(sqlite3* db, const char* sql)
{
    sqlite3_stmt* stmt = nullptr;
    checkSqlite(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), db, "sqlite prepare");
    return stmt;
}

void finish(sqlite3_stmt* stmt)
{
    sqlite3_finalize(stmt);
}

}


SQLiteStateStore::SQLiteStateStore(const std::string& path)
{
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        const std::string message = db_ ? sqlite3_errmsg(db_) : "unknown sqlite error";
        if (db_)
            sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("Failed to open trading state DB: " + message);
    }

    createSchema();
}


SQLiteStateStore::~SQLiteStateStore()
{
    if (db_)
        sqlite3_close(db_);
}


void SQLiteStateStore::exec(const char* sql) const
{
    char* error = nullptr;
    const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &error);
    if (rc == SQLITE_OK)
        return;

    const std::string message = error ? error : sqlite3_errmsg(db_);
    sqlite3_free(error);
    throw std::runtime_error("SQLite state store error: " + message);
}


void SQLiteStateStore::createSchema()
{
    exec("PRAGMA foreign_keys=ON;");
    exec("PRAGMA journal_mode=WAL;");

    exec(
        "CREATE TABLE IF NOT EXISTS runtime_state ("
        "id INTEGER PRIMARY KEY CHECK(id=1),"
        "schema_version INTEGER NOT NULL,"
        "last_bar_close INTEGER NOT NULL,"
        "last_execution INTEGER NOT NULL,"
        "next_order_id INTEGER NOT NULL,"
        "account_cash REAL NOT NULL);"

        "CREATE TABLE IF NOT EXISTS account_positions ("
        "coin TEXT PRIMARY KEY, quantity REAL NOT NULL);"

        "CREATE TABLE IF NOT EXISTS strategy_signals ("
        "strategy_id INTEGER NOT NULL, coin TEXT NOT NULL, signal REAL NOT NULL,"
        "PRIMARY KEY(strategy_id, coin));"

        "CREATE TABLE IF NOT EXISTS strategy_desired_weights ("
        "strategy_id INTEGER NOT NULL, coin TEXT NOT NULL, weight REAL NOT NULL,"
        "PRIMARY KEY(strategy_id, coin));"

        "CREATE TABLE IF NOT EXISTS strategy_virtual_positions ("
        "strategy_id INTEGER NOT NULL, coin TEXT NOT NULL, quantity REAL NOT NULL,"
        "PRIMARY KEY(strategy_id, coin));"

        "CREATE TABLE IF NOT EXISTS pending_plans ("
        "strategy_id INTEGER PRIMARY KEY, plan_timestamp INTEGER NOT NULL,"
        "reference_capital REAL NOT NULL);"

        "CREATE TABLE IF NOT EXISTS pending_plan_decisions ("
        "strategy_id INTEGER NOT NULL, coin TEXT NOT NULL, action INTEGER NOT NULL,"
        "target_weight REAL NOT NULL, PRIMARY KEY(strategy_id, coin));"

        "CREATE TABLE IF NOT EXISTS processed_fill_ids ("
        "fill_id INTEGER PRIMARY KEY);"

        "CREATE TABLE IF NOT EXISTS tracked_orders ("
        "order_id INTEGER PRIMARY KEY, strategy_id INTEGER NOT NULL,"
        "created_at INTEGER NOT NULL, active_from INTEGER NOT NULL,"
        "coin TEXT NOT NULL, side INTEGER NOT NULL, quantity REAL NOT NULL,"
        "status INTEGER NOT NULL, filled_quantity REAL NOT NULL,"
        "updated_at INTEGER NOT NULL, cancel_requested INTEGER NOT NULL,"
        "exchange_order_id TEXT NOT NULL, last_message TEXT NOT NULL);"

        "CREATE TABLE IF NOT EXISTS fills ("
        "fill_id INTEGER PRIMARY KEY, order_id INTEGER NOT NULL, strategy_id INTEGER NOT NULL,"
        "timestamp INTEGER NOT NULL, coin TEXT NOT NULL, side INTEGER NOT NULL,"
        "quantity REAL NOT NULL, price REAL NOT NULL, commission REAL NOT NULL);"
    );
}


void SQLiteStateStore::save(
    const TradingStateSnapshot& snapshot,
    const std::optional<Fill>& newFill
)
{
    exec("BEGIN IMMEDIATE TRANSACTION;");

    try {
        if (newFill) {
            newFill->validate();
            if (newFill->fill_id == 0)
                throw std::invalid_argument("Persisted fills require a stable non-zero fill id");
            sqlite3_stmt* stmt = prepare(db_,
                "INSERT OR IGNORE INTO fills"
                "(fill_id,order_id,strategy_id,timestamp,coin,side,quantity,price,commission)"
                "VALUES(?,?,?,?,?,?,?,?,?);");
            sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(newFill->fill_id));
            sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(newFill->order_id));
            sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(newFill->strategy_id));
            sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(newFill->timestamp));
            bindText(stmt, 5, newFill->coin);
            sqlite3_bind_int(stmt, 6, static_cast<int>(newFill->side));
            sqlite3_bind_double(stmt, 7, newFill->quantity);
            sqlite3_bind_double(stmt, 8, newFill->price);
            sqlite3_bind_double(stmt, 9, newFill->commission);
            checkSqlite(sqlite3_step(stmt), db_, "insert fill");
            finish(stmt);
        }

        exec(
            "DELETE FROM account_positions;"
            "DELETE FROM strategy_signals;"
            "DELETE FROM strategy_desired_weights;"
            "DELETE FROM strategy_virtual_positions;"
            "DELETE FROM pending_plan_decisions;"
            "DELETE FROM pending_plans;"
            "DELETE FROM processed_fill_ids;"
            "DELETE FROM tracked_orders;"
        );

        sqlite3_stmt* runtimeStmt = prepare(db_,
            "INSERT INTO runtime_state"
            "(id,schema_version,last_bar_close,last_execution,next_order_id,account_cash)"
            "VALUES(1,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET "
            "schema_version=excluded.schema_version,last_bar_close=excluded.last_bar_close,"
            "last_execution=excluded.last_execution,next_order_id=excluded.next_order_id,"
            "account_cash=excluded.account_cash;");
        sqlite3_bind_int(runtimeStmt, 1, static_cast<int>(snapshot.schema_version));
        sqlite3_bind_int64(runtimeStmt, 2, snapshot.last_bar_close_timestamp);
        sqlite3_bind_int64(runtimeStmt, 3, snapshot.last_execution_timestamp);
        sqlite3_bind_int64(runtimeStmt, 4, snapshot.next_order_id);
        sqlite3_bind_double(runtimeStmt, 5, snapshot.account_cash);
        checkSqlite(sqlite3_step(runtimeStmt), db_, "save runtime state");
        finish(runtimeStmt);

        sqlite3_stmt* positionStmt = prepare(db_,
            "INSERT INTO account_positions(coin,quantity) VALUES(?,?);");
        for (const auto& [coin, quantity] : snapshot.account_positions) {
            bindText(positionStmt, 1, coin);
            sqlite3_bind_double(positionStmt, 2, quantity);
            checkSqlite(sqlite3_step(positionStmt), db_, "save account position");
            sqlite3_reset(positionStmt);
            sqlite3_clear_bindings(positionStmt);
        }
        finish(positionStmt);

        sqlite3_stmt* signalStmt = prepare(db_,
            "INSERT INTO strategy_signals(strategy_id,coin,signal) VALUES(?,?,?);");
        sqlite3_stmt* weightStmt = prepare(db_,
            "INSERT INTO strategy_desired_weights(strategy_id,coin,weight) VALUES(?,?,?);");
        sqlite3_stmt* virtualStmt = prepare(db_,
            "INSERT INTO strategy_virtual_positions(strategy_id,coin,quantity) VALUES(?,?,?);");

        for (const StrategyStateSnapshot& strategy : snapshot.strategies) {
            for (const auto& [coin, signal] : strategy.signals) {
                sqlite3_bind_int64(signalStmt, 1, strategy.strategy_id);
                bindText(signalStmt, 2, coin);
                sqlite3_bind_double(signalStmt, 3, signal);
                checkSqlite(sqlite3_step(signalStmt), db_, "save strategy signal");
                sqlite3_reset(signalStmt); sqlite3_clear_bindings(signalStmt);
            }
            for (const auto& [coin, weight] : strategy.desired_weights) {
                sqlite3_bind_int64(weightStmt, 1, strategy.strategy_id);
                bindText(weightStmt, 2, coin);
                sqlite3_bind_double(weightStmt, 3, weight);
                checkSqlite(sqlite3_step(weightStmt), db_, "save desired weight");
                sqlite3_reset(weightStmt); sqlite3_clear_bindings(weightStmt);
            }
            for (const auto& [coin, quantity] : strategy.virtual_positions) {
                sqlite3_bind_int64(virtualStmt, 1, strategy.strategy_id);
                bindText(virtualStmt, 2, coin);
                sqlite3_bind_double(virtualStmt, 3, quantity);
                checkSqlite(sqlite3_step(virtualStmt), db_, "save virtual position");
                sqlite3_reset(virtualStmt); sqlite3_clear_bindings(virtualStmt);
            }
        }
        finish(signalStmt); finish(weightStmt); finish(virtualStmt);

        sqlite3_stmt* planStmt = prepare(db_,
            "INSERT INTO pending_plans(strategy_id,plan_timestamp,reference_capital) VALUES(?,?,?);");
        sqlite3_stmt* decisionStmt = prepare(db_,
            "INSERT INTO pending_plan_decisions(strategy_id,coin,action,target_weight) VALUES(?,?,?,?);");
        for (const PendingPlanSnapshot& pending : snapshot.pending_plans) {
            sqlite3_bind_int64(planStmt, 1, pending.strategy_id);
            sqlite3_bind_int64(planStmt, 2, pending.plan.timestamp());
            sqlite3_bind_double(planStmt, 3, pending.plan.referenceCapital());
            checkSqlite(sqlite3_step(planStmt), db_, "save pending plan");
            sqlite3_reset(planStmt); sqlite3_clear_bindings(planStmt);

            for (const auto& [coin, decision] : pending.plan.values()) {
                sqlite3_bind_int64(decisionStmt, 1, pending.strategy_id);
                bindText(decisionStmt, 2, coin);
                sqlite3_bind_int(decisionStmt, 3, static_cast<int>(decision.action));
                sqlite3_bind_double(decisionStmt, 4, decision.target_weight);
                checkSqlite(sqlite3_step(decisionStmt), db_, "save pending decision");
                sqlite3_reset(decisionStmt); sqlite3_clear_bindings(decisionStmt);
            }
        }
        finish(planStmt); finish(decisionStmt);

        sqlite3_stmt* orderStmt = prepare(db_,
            "INSERT INTO tracked_orders"
            "(order_id,strategy_id,created_at,active_from,coin,side,quantity,status,"
            "filled_quantity,updated_at,cancel_requested,exchange_order_id,last_message)"
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?);");
        for (const TrackedOrder& order : snapshot.orders) {
            sqlite3_bind_int64(orderStmt, 1, order.request.order_id);
            sqlite3_bind_int64(orderStmt, 2, order.request.strategy_id);
            sqlite3_bind_int64(orderStmt, 3, order.request.created_at);
            sqlite3_bind_int64(orderStmt, 4, order.request.active_from);
            bindText(orderStmt, 5, order.request.coin);
            sqlite3_bind_int(orderStmt, 6, static_cast<int>(order.request.side));
            sqlite3_bind_double(orderStmt, 7, order.request.quantity);
            sqlite3_bind_int(orderStmt, 8, static_cast<int>(order.status));
            sqlite3_bind_double(orderStmt, 9, order.filled_quantity);
            sqlite3_bind_int64(orderStmt, 10, order.updated_at);
            sqlite3_bind_int(orderStmt, 11, order.cancel_requested ? 1 : 0);
            bindText(orderStmt, 12, order.exchange_order_id);
            bindText(orderStmt, 13, order.last_message);
            checkSqlite(sqlite3_step(orderStmt), db_, "save tracked order");
            sqlite3_reset(orderStmt); sqlite3_clear_bindings(orderStmt);
        }
        finish(orderStmt);

        sqlite3_stmt* processedFillStmt = prepare(db_,
            "INSERT INTO processed_fill_ids(fill_id) VALUES(?);");
        for (const FillID fillId : snapshot.processed_fill_ids) {
            sqlite3_bind_int64(processedFillStmt, 1, static_cast<sqlite3_int64>(fillId));
            checkSqlite(sqlite3_step(processedFillStmt), db_, "save processed fill id");
            sqlite3_reset(processedFillStmt);
            sqlite3_clear_bindings(processedFillStmt);
        }
        finish(processedFillStmt);

        exec("COMMIT;");
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        throw;
    }
}


std::optional<TradingStateSnapshot> SQLiteStateStore::load() const
{
    TradingStateSnapshot snapshot;

    sqlite3_stmt* runtimeStmt = prepare(db_,
        "SELECT schema_version,last_bar_close,last_execution,next_order_id,account_cash "
        "FROM runtime_state WHERE id=1;");
    if (sqlite3_step(runtimeStmt) != SQLITE_ROW) {
        finish(runtimeStmt);
        return std::nullopt;
    }

    snapshot.schema_version = static_cast<unsigned int>(sqlite3_column_int(runtimeStmt, 0));
    snapshot.last_bar_close_timestamp = static_cast<Timestamp>(sqlite3_column_int64(runtimeStmt, 1));
    snapshot.last_execution_timestamp = static_cast<Timestamp>(sqlite3_column_int64(runtimeStmt, 2));
    snapshot.next_order_id = static_cast<OrderID>(sqlite3_column_int64(runtimeStmt, 3));
    snapshot.account_cash = sqlite3_column_double(runtimeStmt, 4);
    finish(runtimeStmt);

    if (snapshot.schema_version != TradingStateSnapshot::CURRENT_SCHEMA_VERSION)
        throw std::runtime_error("Unsupported trading state schema version");

    sqlite3_stmt* stmt = prepare(db_, "SELECT coin,quantity FROM account_positions;");
    while (sqlite3_step(stmt) == SQLITE_ROW)
        snapshot.account_positions.emplace(
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)),
            sqlite3_column_double(stmt, 1));
    finish(stmt);

    std::unordered_map<StrategyID, StrategyStateSnapshot> strategyMap;
    const auto loadStrategyValues = [&](const char* sql, int field) {
        sqlite3_stmt* valueStmt = prepare(db_, sql);
        while (sqlite3_step(valueStmt) == SQLITE_ROW) {
            const StrategyID id = static_cast<StrategyID>(sqlite3_column_int64(valueStmt, 0));
            auto& strategy = strategyMap[id];
            strategy.strategy_id = id;
            const Coin coin = reinterpret_cast<const char*>(sqlite3_column_text(valueStmt, 1));
            const double value = sqlite3_column_double(valueStmt, 2);
            if (field == 0) strategy.signals.emplace(coin, value);
            else if (field == 1) strategy.desired_weights.emplace(coin, value);
            else strategy.virtual_positions.emplace(coin, value);
        }
        finish(valueStmt);
    };
    loadStrategyValues("SELECT strategy_id,coin,signal FROM strategy_signals;", 0);
    loadStrategyValues("SELECT strategy_id,coin,weight FROM strategy_desired_weights;", 1);
    loadStrategyValues("SELECT strategy_id,coin,quantity FROM strategy_virtual_positions;", 2);
    for (auto& [id, strategy] : strategyMap) {
        (void)id;
        snapshot.strategies.push_back(std::move(strategy));
    }

    std::unordered_map<StrategyID, PendingPlanSnapshot> planMap;
    stmt = prepare(db_, "SELECT strategy_id,plan_timestamp,reference_capital FROM pending_plans;");
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const StrategyID id = static_cast<StrategyID>(sqlite3_column_int64(stmt, 0));
        PendingPlanSnapshot pending;
        pending.strategy_id = id;
        pending.plan = RebalancePlan(
            static_cast<Timestamp>(sqlite3_column_int64(stmt, 1)),
            sqlite3_column_double(stmt, 2));
        planMap.emplace(id, std::move(pending));
    }
    finish(stmt);

    stmt = prepare(db_,
        "SELECT strategy_id,coin,action,target_weight FROM pending_plan_decisions;");
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const StrategyID id = static_cast<StrategyID>(sqlite3_column_int64(stmt, 0));
        const Coin coin = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const auto action = static_cast<RebalanceAction>(sqlite3_column_int(stmt, 2));
        const double weight = sqlite3_column_double(stmt, 3);
        auto planIt = planMap.find(id);
        if (planIt == planMap.end())
            throw std::runtime_error("Pending decision has no parent plan");

        RebalanceDecision decision;
        if (action == RebalanceAction::Flat) decision = RebalanceDecision::flat();
        else if (action == RebalanceAction::TargetWeight) decision = RebalanceDecision::targetWeight(weight);
        else decision = RebalanceDecision::hold();
        planIt->second.plan.set(coin, decision);
    }
    finish(stmt);
    for (auto& [id, pending] : planMap) {
        (void)id;
        snapshot.pending_plans.push_back(std::move(pending));
    }

    stmt = prepare(db_,
        "SELECT order_id,strategy_id,created_at,active_from,coin,side,quantity,status,"
        "filled_quantity,updated_at,cancel_requested,exchange_order_id,last_message "
        "FROM tracked_orders ORDER BY order_id;");
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ExecutionOrder request(
            static_cast<OrderID>(sqlite3_column_int64(stmt, 0)),
            static_cast<StrategyID>(sqlite3_column_int64(stmt, 1)),
            static_cast<Timestamp>(sqlite3_column_int64(stmt, 2)),
            static_cast<Timestamp>(sqlite3_column_int64(stmt, 3)),
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4)),
            static_cast<OrderSide>(sqlite3_column_int(stmt, 5)),
            sqlite3_column_double(stmt, 6));

        TrackedOrder order(std::move(request));
        order.status = static_cast<ExecutionOrderStatus>(sqlite3_column_int(stmt, 7));
        order.filled_quantity = sqlite3_column_double(stmt, 8);
        order.updated_at = static_cast<Timestamp>(sqlite3_column_int64(stmt, 9));
        order.cancel_requested = sqlite3_column_int(stmt, 10) != 0;
        order.exchange_order_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
        order.last_message = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 12));
        snapshot.orders.push_back(std::move(order));
    }
    finish(stmt);

    stmt = prepare(db_, "SELECT fill_id FROM processed_fill_ids ORDER BY fill_id;");
    while (sqlite3_step(stmt) == SQLITE_ROW)
        snapshot.processed_fill_ids.push_back(static_cast<FillID>(sqlite3_column_int64(stmt, 0)));
    finish(stmt);

    return snapshot;
}


std::vector<Fill> SQLiteStateStore::loadFills() const
{
    std::vector<Fill> fills;
    sqlite3_stmt* stmt = prepare(db_,
        "SELECT fill_id,order_id,strategy_id,timestamp,coin,side,quantity,price,commission "
        "FROM fills ORDER BY fill_id;");

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Fill fill;
        fill.fill_id = static_cast<FillID>(sqlite3_column_int64(stmt, 0));
        fill.order_id = static_cast<OrderID>(sqlite3_column_int64(stmt, 1));
        fill.strategy_id = static_cast<StrategyID>(sqlite3_column_int64(stmt, 2));
        fill.timestamp = static_cast<Timestamp>(sqlite3_column_int64(stmt, 3));
        fill.coin = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        fill.side = static_cast<OrderSide>(sqlite3_column_int(stmt, 5));
        fill.quantity = sqlite3_column_double(stmt, 6);
        fill.price = sqlite3_column_double(stmt, 7);
        fill.commission = sqlite3_column_double(stmt, 8);
        fill.validate();
        fills.push_back(std::move(fill));
    }
    finish(stmt);
    return fills;
}
