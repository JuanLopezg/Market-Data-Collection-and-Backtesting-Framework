#include "postgres_state_store.h"

#include <array>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>

#include <libpq-fe.h>
#include <nlohmann/json.hpp>


namespace {

using json = nlohmann::json;


void requireConnection(PGconn* connection, const char* context)
{
    if (connection && PQstatus(connection) == CONNECTION_OK)
        return;

    const std::string detail = connection ? PQerrorMessage(connection) : "null connection";
    throw std::runtime_error(std::string(context) + ": " + detail);
}


PGresult* exec(PGconn* connection, const char* sql, ExecStatusType expected = PGRES_COMMAND_OK)
{
    PGresult* result = PQexec(connection, sql);
    if (result && PQresultStatus(result) == expected)
        return result;

    const std::string detail = result ? PQresultErrorMessage(result) : PQerrorMessage(connection);
    if (result)
        PQclear(result);
    throw std::runtime_error("PostgreSQL state store error: " + detail);
}


PGresult* execParams(
    PGconn* connection,
    const char* sql,
    int count,
    const char* const* values,
    ExecStatusType expected = PGRES_COMMAND_OK
)
{
    PGresult* result = PQexecParams(
        connection,
        sql,
        count,
        nullptr,
        values,
        nullptr,
        nullptr,
        0
    );

    if (result && PQresultStatus(result) == expected)
        return result;

    const std::string detail = result ? PQresultErrorMessage(result) : PQerrorMessage(connection);
    if (result)
        PQclear(result);
    throw std::runtime_error("PostgreSQL state store error: " + detail);
}


void clear(PGresult* result)
{
    if (result)
        PQclear(result);
}


json encodeFill(const Fill& fill)
{
    return {
        {"fill_id", fill.fill_id},
        {"order_id", fill.order_id},
        {"strategy_id", fill.strategy_id},
        {"timestamp", fill.timestamp},
        {"coin", fill.coin},
        {"side", static_cast<int>(fill.side)},
        {"quantity", fill.quantity},
        {"price", fill.price},
        {"commission", fill.commission}
    };
}


Fill decodeFill(const json& value)
{
    Fill fill;
    fill.fill_id = value.at("fill_id").get<FillID>();
    fill.order_id = value.at("order_id").get<OrderID>();
    fill.strategy_id = value.at("strategy_id").get<StrategyID>();
    fill.timestamp = value.at("timestamp").get<Timestamp>();
    fill.coin = value.at("coin").get<Coin>();
    fill.side = static_cast<OrderSide>(value.at("side").get<int>());
    fill.quantity = value.at("quantity").get<double>();
    fill.price = value.at("price").get<double>();
    fill.commission = value.at("commission").get<double>();
    fill.validate();
    return fill;
}


json encodeSnapshot(const TradingStateSnapshot& snapshot)
{
    json strategies = json::array();
    for (const StrategyStateSnapshot& strategy : snapshot.strategies) {
        strategies.push_back({
            {"strategy_id", strategy.strategy_id},
            {"signals", strategy.signals},
            {"desired_weights", strategy.desired_weights},
            {"virtual_positions", strategy.virtual_positions}
        });
    }

    json pendingPlans = json::array();
    for (const PendingPlanSnapshot& pending : snapshot.pending_plans) {
        json decisions = json::array();
        for (const auto& [coin, decision] : pending.plan.values()) {
            decisions.push_back({
                {"coin", coin},
                {"action", static_cast<int>(decision.action)},
                {"target_weight", decision.target_weight}
            });
        }

        pendingPlans.push_back({
            {"strategy_id", pending.strategy_id},
            {"timestamp", pending.plan.timestamp()},
            {"reference_capital", pending.plan.referenceCapital()},
            {"decisions", std::move(decisions)}
        });
    }

    json orders = json::array();
    for (const TrackedOrder& order : snapshot.orders) {
        orders.push_back({
            {"order_id", order.request.order_id},
            {"strategy_id", order.request.strategy_id},
            {"created_at", order.request.created_at},
            {"active_from", order.request.active_from},
            {"coin", order.request.coin},
            {"side", static_cast<int>(order.request.side)},
            {"quantity", order.request.quantity},
            {"status", static_cast<int>(order.status)},
            {"filled_quantity", order.filled_quantity},
            {"updated_at", order.updated_at},
            {"cancel_requested", order.cancel_requested},
            {"exchange_order_id", order.exchange_order_id},
            {"last_message", order.last_message}
        });
    }

    return {
        {"schema_version", snapshot.schema_version},
        {"last_bar_close_timestamp", snapshot.last_bar_close_timestamp},
        {"last_execution_timestamp", snapshot.last_execution_timestamp},
        {"next_order_id", snapshot.next_order_id},
        {"account_cash", snapshot.account_cash},
        {"account_positions", snapshot.account_positions},
        {"strategies", std::move(strategies)},
        {"pending_plans", std::move(pendingPlans)},
        {"orders", std::move(orders)},
        {"processed_fill_ids", snapshot.processed_fill_ids}
    };
}


TradingStateSnapshot decodeSnapshot(const json& value)
{
    TradingStateSnapshot snapshot;
    snapshot.schema_version = value.at("schema_version").get<unsigned int>();
    snapshot.last_bar_close_timestamp = value.at("last_bar_close_timestamp").get<Timestamp>();
    snapshot.last_execution_timestamp = value.at("last_execution_timestamp").get<Timestamp>();
    snapshot.next_order_id = value.at("next_order_id").get<OrderID>();
    snapshot.account_cash = value.at("account_cash").get<double>();
    snapshot.account_positions = value.at("account_positions").get<std::unordered_map<Coin, double>>();

    for (const json& entry : value.at("strategies")) {
        StrategyStateSnapshot strategy;
        strategy.strategy_id = entry.at("strategy_id").get<StrategyID>();
        strategy.signals = entry.at("signals").get<std::unordered_map<Coin, double>>();
        strategy.desired_weights = entry.at("desired_weights").get<std::unordered_map<Coin, double>>();
        strategy.virtual_positions = entry.at("virtual_positions").get<std::unordered_map<Coin, double>>();
        snapshot.strategies.push_back(std::move(strategy));
    }

    for (const json& entry : value.at("pending_plans")) {
        RebalancePlan plan(
            entry.at("timestamp").get<Timestamp>(),
            entry.at("reference_capital").get<double>()
        );

        for (const json& decisionValue : entry.at("decisions")) {
            RebalanceDecision decision;
            decision.action = static_cast<RebalanceAction>(decisionValue.at("action").get<int>());
            decision.target_weight = decisionValue.at("target_weight").get<double>();
            plan.set(decisionValue.at("coin").get<Coin>(), decision);
        }

        snapshot.pending_plans.push_back({
            entry.at("strategy_id").get<StrategyID>(),
            std::move(plan)
        });
    }

    for (const json& entry : value.at("orders")) {
        ExecutionOrder request(
            entry.at("order_id").get<OrderID>(),
            entry.at("strategy_id").get<StrategyID>(),
            entry.at("created_at").get<Timestamp>(),
            entry.at("active_from").get<Timestamp>(),
            entry.at("coin").get<Coin>(),
            static_cast<OrderSide>(entry.at("side").get<int>()),
            entry.at("quantity").get<double>()
        );

        TrackedOrder order(std::move(request));
        order.status = static_cast<ExecutionOrderStatus>(entry.at("status").get<int>());
        order.filled_quantity = entry.at("filled_quantity").get<double>();
        order.updated_at = entry.at("updated_at").get<Timestamp>();
        order.cancel_requested = entry.at("cancel_requested").get<bool>();
        order.exchange_order_id = entry.at("exchange_order_id").get<std::string>();
        order.last_message = entry.at("last_message").get<std::string>();
        snapshot.orders.push_back(std::move(order));
    }

    snapshot.processed_fill_ids = value.at("processed_fill_ids").get<std::vector<FillID>>();
    return snapshot;
}

}


struct PostgresStateStore::Impl {
    PGconn* connection = nullptr;

    explicit Impl(const std::string& connectionString)
        : connection(PQconnectdb(connectionString.c_str()))
    {
        requireConnection(connection, "Failed to connect PostgreSQL state store");

        PGresult* result = exec(
            connection,
            "CREATE TABLE IF NOT EXISTS trading_runtime_state ("
            "singleton BOOLEAN PRIMARY KEY DEFAULT TRUE CHECK(singleton),"
            "schema_version INTEGER NOT NULL,"
            "snapshot JSONB NOT NULL,"
            "updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW());"
            "CREATE TABLE IF NOT EXISTS trading_fills ("
            "fill_id BIGINT PRIMARY KEY,"
            "order_id BIGINT NOT NULL,"
            "strategy_id BIGINT NOT NULL,"
            "timestamp BIGINT NOT NULL,"
            "coin TEXT NOT NULL,"
            "side INTEGER NOT NULL,"
            "quantity DOUBLE PRECISION NOT NULL,"
            "price DOUBLE PRECISION NOT NULL,"
            "commission DOUBLE PRECISION NOT NULL);"
            "CREATE INDEX IF NOT EXISTS trading_fills_timestamp_idx "
            "ON trading_fills(timestamp, fill_id);"
        );
        clear(result);
    }

    ~Impl()
    {
        if (connection)
            PQfinish(connection);
    }
};


PostgresStateStore::PostgresStateStore(const std::string& connectionString)
    : impl_(std::make_unique<Impl>(connectionString))
{}


PostgresStateStore::~PostgresStateStore() = default;


void PostgresStateStore::save(
    const TradingStateSnapshot& snapshot,
    const std::optional<Fill>& newFill
)
{
    PGresult* result = exec(impl_->connection, "BEGIN;");
    clear(result);

    try {
        if (newFill) {
            newFill->validate();
            if (newFill->fill_id == 0)
                throw std::invalid_argument("Persisted fills require a stable non-zero fill id");

            const std::array<std::string, 9> parameters{
                std::to_string(newFill->fill_id),
                std::to_string(newFill->order_id),
                std::to_string(newFill->strategy_id),
                std::to_string(newFill->timestamp),
                newFill->coin,
                std::to_string(static_cast<int>(newFill->side)),
                std::to_string(newFill->quantity),
                std::to_string(newFill->price),
                std::to_string(newFill->commission)
            };
            std::array<const char*, 9> values{};
            for (std::size_t i = 0; i < parameters.size(); ++i)
                values[i] = parameters[i].c_str();

            result = execParams(
                impl_->connection,
                "INSERT INTO trading_fills"
                "(fill_id,order_id,strategy_id,timestamp,coin,side,quantity,price,commission) "
                "VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9) ON CONFLICT(fill_id) DO NOTHING;",
                static_cast<int>(values.size()),
                values.data()
            );
            clear(result);
        }

        const std::string snapshotJson = encodeSnapshot(snapshot).dump();
        const std::string schemaVersion = std::to_string(snapshot.schema_version);
        const std::array<const char*, 2> values{schemaVersion.c_str(), snapshotJson.c_str()};

        result = execParams(
            impl_->connection,
            "INSERT INTO trading_runtime_state(singleton,schema_version,snapshot,updated_at) "
            "VALUES(TRUE,$1,$2::jsonb,NOW()) "
            "ON CONFLICT(singleton) DO UPDATE SET "
            "schema_version=EXCLUDED.schema_version,snapshot=EXCLUDED.snapshot,updated_at=NOW();",
            static_cast<int>(values.size()),
            values.data()
        );
        clear(result);

        result = exec(impl_->connection, "COMMIT;");
        clear(result);
    } catch (...) {
        PGresult* rollback = PQexec(impl_->connection, "ROLLBACK;");
        clear(rollback);
        throw;
    }
}


std::optional<TradingStateSnapshot> PostgresStateStore::load() const
{
    PGresult* result = exec(
        impl_->connection,
        "SELECT snapshot::text FROM trading_runtime_state WHERE singleton=TRUE;",
        PGRES_TUPLES_OK
    );

    if (PQntuples(result) == 0) {
        clear(result);
        return std::nullopt;
    }

    try {
        TradingStateSnapshot snapshot = decodeSnapshot(json::parse(PQgetvalue(result, 0, 0)));
        clear(result);
        return snapshot;
    } catch (...) {
        clear(result);
        throw;
    }
}


std::vector<Fill> PostgresStateStore::loadFills() const
{
    PGresult* result = exec(
        impl_->connection,
        "SELECT fill_id,order_id,strategy_id,timestamp,coin,side,quantity,price,commission "
        "FROM trading_fills ORDER BY timestamp,fill_id;",
        PGRES_TUPLES_OK
    );

    std::vector<Fill> fills;
    fills.reserve(static_cast<std::size_t>(PQntuples(result)));

    try {
        for (int row = 0; row < PQntuples(result); ++row) {
            Fill fill;
            fill.fill_id = std::strtoull(PQgetvalue(result, row, 0), nullptr, 10);
            fill.order_id = static_cast<OrderID>(std::strtoul(PQgetvalue(result, row, 1), nullptr, 10));
            fill.strategy_id = static_cast<StrategyID>(std::strtoul(PQgetvalue(result, row, 2), nullptr, 10));
            fill.timestamp = static_cast<Timestamp>(std::strtoul(PQgetvalue(result, row, 3), nullptr, 10));
            fill.coin = PQgetvalue(result, row, 4);
            fill.side = static_cast<OrderSide>(std::strtol(PQgetvalue(result, row, 5), nullptr, 10));
            fill.quantity = std::strtod(PQgetvalue(result, row, 6), nullptr);
            fill.price = std::strtod(PQgetvalue(result, row, 7), nullptr);
            fill.commission = std::strtod(PQgetvalue(result, row, 8), nullptr);
            fill.validate();
            fills.push_back(std::move(fill));
        }

        clear(result);
        return fills;
    } catch (...) {
        clear(result);
        throw;
    }
}
