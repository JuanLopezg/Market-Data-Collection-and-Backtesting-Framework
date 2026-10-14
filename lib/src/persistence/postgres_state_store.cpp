// Store the operational snapshot as JSONB and keep fills in a separate audit table.
// A transaction commits the optional fill and its resulting snapshot together.

#include "postgres_state_store.h"

#include <array>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include <libpq-fe.h>
#include <nlohmann/json.hpp>


namespace {

using json = nlohmann::json;


// Use enough significant digits for a double to survive a text/database round trip.
// Default stream precision would change quantities or cash on restart.
std::string encodeDoubleForPostgres(double value)
{
    std::ostringstream stream;
    stream << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return stream.str();
}


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
        json entry = {
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
        };
        // A recovered unsent order is published again; keep what the venue needs.
        if (order.request.reference_price > 0.0)
            entry["reference_price"] = order.request.reference_price;
        if (order.request.fill_at_reference_price)
            entry["fill_at_reference_price"] = true;
        orders.push_back(std::move(entry));
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
        request.reference_price = entry.value("reference_price", 0.0);
        request.fill_at_reference_price = entry.value("fill_at_reference_price", false);

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
            // No "relation already exists, skipping" NOTICE per table on every start.
            "SET client_min_messages TO WARNING;"
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
            // Accounting projection written by the runtime (see PositionLedger).
            "CREATE TABLE IF NOT EXISTS trading_open_positions ("
            "coin TEXT PRIMARY KEY,"
            "side TEXT NOT NULL,"
            "opened_at BIGINT NOT NULL,"
            "quantity DOUBLE PRECISION NOT NULL,"
            "max_quantity DOUBLE PRECISION NOT NULL,"
            "entry_price DOUBLE PRECISION NOT NULL,"
            "entry_value DOUBLE PRECISION NOT NULL,"
            "exit_value DOUBLE PRECISION NOT NULL,"
            "exit_quantity DOUBLE PRECISION NOT NULL,"
            "gross_pnl DOUBLE PRECISION NOT NULL,"
            "fees DOUBLE PRECISION NOT NULL,"
            "funding DOUBLE PRECISION NOT NULL);"
            "CREATE TABLE IF NOT EXISTS trading_closed_positions ("
            "closing_fill_id BIGINT PRIMARY KEY,"
            "coin TEXT NOT NULL,"
            "side TEXT NOT NULL,"
            "opened_at BIGINT NOT NULL,"
            "closed_at BIGINT NOT NULL,"
            "max_quantity DOUBLE PRECISION NOT NULL,"
            "entry_price DOUBLE PRECISION NOT NULL,"
            "exit_price DOUBLE PRECISION NOT NULL,"
            "entry_value DOUBLE PRECISION NOT NULL,"
            "exit_value DOUBLE PRECISION NOT NULL,"
            "exit_quantity DOUBLE PRECISION NOT NULL,"
            "gross_pnl DOUBLE PRECISION NOT NULL,"
            "fees DOUBLE PRECISION NOT NULL,"
            "funding DOUBLE PRECISION NOT NULL,"
            "net_pnl DOUBLE PRECISION NOT NULL);"
            "CREATE TABLE IF NOT EXISTS trading_equity_daily ("
            "date BIGINT PRIMARY KEY,"
            "cash DOUBLE PRECISION NOT NULL,"
            "positions_value DOUBLE PRECISION NOT NULL,"
            "equity DOUBLE PRECISION NOT NULL,"
            "realized_gross_pnl DOUBLE PRECISION NOT NULL,"
            "fees DOUBLE PRECISION NOT NULL,"
            "funding DOUBLE PRECISION NOT NULL,"
            "open_positions INTEGER NOT NULL,"
            "updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW());"
            // Added 2026-10-14 (real venues; 0 in PAPER). Older releases ignore them.
            "ALTER TABLE trading_equity_daily "
            "ADD COLUMN IF NOT EXISTS capital_flows DOUBLE PRECISION NOT NULL DEFAULT 0,"
            "ADD COLUMN IF NOT EXISTS interest DOUBLE PRECISION NOT NULL DEFAULT 0,"
            "ADD COLUMN IF NOT EXISTS collateral_usd DOUBLE PRECISION NOT NULL DEFAULT 0;"
            // Venue collateral fixed for each day's sizing snapshot.
            "CREATE TABLE IF NOT EXISTS trading_daily_collateral ("
            "date BIGINT PRIMARY KEY,"
            "collateral_usd DOUBLE PRECISION NOT NULL,"
            "recorded_at TIMESTAMPTZ NOT NULL DEFAULT NOW());"
            // Funding payments confirmed by a real venue (none in the PAPER simulation).
            "CREATE TABLE IF NOT EXISTS trading_funding ("
            "payment_id TEXT PRIMARY KEY,"
            "timestamp BIGINT NOT NULL,"
            "coin TEXT NOT NULL,"
            "amount DOUBLE PRECISION NOT NULL,"
            "recorded_at TIMESTAMPTZ NOT NULL DEFAULT NOW());"
            // Venue cash movements applied to the account (fees after the fill, funding,
            // interest, capital flows); the primary key makes each one apply once.
            "CREATE TABLE IF NOT EXISTS trading_venue_costs ("
            "cost_id TEXT PRIMARY KEY,"
            "kind TEXT NOT NULL,"
            "timestamp BIGINT NOT NULL,"
            "coin TEXT NOT NULL,"
            "fill_id BIGINT NOT NULL,"
            "amount DOUBLE PRECISION NOT NULL,"
            "recorded_at TIMESTAMPTZ NOT NULL DEFAULT NOW());"
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
    snapshot.requireMarketOnly();
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
                encodeDoubleForPostgres(newFill->quantity),
                encodeDoubleForPostgres(newFill->price),
                encodeDoubleForPostgres(newFill->commission)
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


// An absent singleton row means no saved runtime yet, not a zero-valued account.
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


// Accounting projection --------------------------------------------------------------

namespace {

// Runs `body` inside BEGIN/COMMIT, rolling back on any error.
template <typename Body>
void inTransaction(PGconn* connection, Body body)
{
    clear(exec(connection, "BEGIN;"));
    try {
        body();
        clear(exec(connection, "COMMIT;"));
    } catch (...) {
        clear(PQexec(connection, "ROLLBACK;"));
        throw;
    }
}


void writeOpenPositions(PGconn* connection, const PositionLedger& ledger)
{
    clear(exec(connection, "DELETE FROM trading_open_positions;"));
    for (const auto& entry : ledger.openPositions()) {
        const PositionLot& lot = entry.second;
        const std::array<std::string, 12> parameters{
            lot.coin, lot.sideName(), std::to_string(lot.opened_at),
            encodeDoubleForPostgres(lot.quantity), encodeDoubleForPostgres(lot.max_quantity),
            encodeDoubleForPostgres(lot.entry_price), encodeDoubleForPostgres(lot.entry_value),
            encodeDoubleForPostgres(lot.exit_value), encodeDoubleForPostgres(lot.exit_quantity),
            encodeDoubleForPostgres(lot.gross_pnl), encodeDoubleForPostgres(lot.fees),
            encodeDoubleForPostgres(lot.funding)
        };
        std::array<const char*, 12> values{};
        for (std::size_t i = 0; i < parameters.size(); ++i)
            values[i] = parameters[i].c_str();
        clear(execParams(
            connection,
            "INSERT INTO trading_open_positions(coin,side,opened_at,quantity,max_quantity,entry_price,"
            "entry_value,exit_value,exit_quantity,gross_pnl,fees,funding) "
            "VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12);",
            static_cast<int>(values.size()), values.data()));
    }
}


void writeClosedPosition(PGconn* connection, const PositionLot& lot)
{
    const std::array<std::string, 15> parameters{
        std::to_string(lot.closing_fill_id), lot.coin, lot.sideName(),
        std::to_string(lot.opened_at), std::to_string(lot.closed_at),
        encodeDoubleForPostgres(lot.max_quantity), encodeDoubleForPostgres(lot.entry_price),
        encodeDoubleForPostgres(lot.exitPrice()), encodeDoubleForPostgres(lot.entry_value),
        encodeDoubleForPostgres(lot.exit_value), encodeDoubleForPostgres(lot.exit_quantity),
        encodeDoubleForPostgres(lot.gross_pnl), encodeDoubleForPostgres(lot.fees),
        encodeDoubleForPostgres(lot.funding), encodeDoubleForPostgres(lot.netPnl())
    };
    std::array<const char*, 15> values{};
    for (std::size_t i = 0; i < parameters.size(); ++i)
        values[i] = parameters[i].c_str();
    clear(execParams(
        connection,
        "INSERT INTO trading_closed_positions(closing_fill_id,coin,side,opened_at,closed_at,max_quantity,"
        "entry_price,exit_price,entry_value,exit_value,exit_quantity,gross_pnl,fees,funding,net_pnl) "
        "VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15) "
        "ON CONFLICT(closing_fill_id) DO UPDATE SET coin=EXCLUDED.coin,side=EXCLUDED.side,"
        "opened_at=EXCLUDED.opened_at,closed_at=EXCLUDED.closed_at,max_quantity=EXCLUDED.max_quantity,"
        "entry_price=EXCLUDED.entry_price,exit_price=EXCLUDED.exit_price,entry_value=EXCLUDED.entry_value,"
        "exit_value=EXCLUDED.exit_value,exit_quantity=EXCLUDED.exit_quantity,gross_pnl=EXCLUDED.gross_pnl,fees=EXCLUDED.fees,"
        "funding=EXCLUDED.funding,net_pnl=EXCLUDED.net_pnl;",
        static_cast<int>(values.size()), values.data()));
}

}


void PostgresStateStore::replaceLedger(const PositionLedger& ledger)
{
    inTransaction(impl_->connection, [&] {
        clear(exec(impl_->connection, "DELETE FROM trading_closed_positions;"));
        for (const PositionLot& lot : ledger.closedPositions())
            writeClosedPosition(impl_->connection, lot);
        writeOpenPositions(impl_->connection, ledger);
    });
}


void PostgresStateStore::saveLedger(const PositionLedger& ledger, const std::vector<PositionLot>& newlyClosed)
{
    inTransaction(impl_->connection, [&] {
        for (const PositionLot& lot : newlyClosed)
            writeClosedPosition(impl_->connection, lot);
        writeOpenPositions(impl_->connection, ledger);
    });
}


void PostgresStateStore::saveEquityDay(const EquityDay& day)
{
    const std::array<std::string, 11> parameters{
        std::to_string(day.date), encodeDoubleForPostgres(day.cash),
        encodeDoubleForPostgres(day.positions_value), encodeDoubleForPostgres(day.equity),
        encodeDoubleForPostgres(day.realized_gross_pnl), encodeDoubleForPostgres(day.fees),
        encodeDoubleForPostgres(day.funding), std::to_string(day.open_positions),
        encodeDoubleForPostgres(day.capital_flows), encodeDoubleForPostgres(day.interest),
        encodeDoubleForPostgres(day.collateral_usd)
    };
    std::array<const char*, 11> values{};
    for (std::size_t i = 0; i < parameters.size(); ++i)
        values[i] = parameters[i].c_str();
    clear(execParams(
        impl_->connection,
        "INSERT INTO trading_equity_daily(date,cash,positions_value,equity,realized_gross_pnl,fees,funding,"
        "open_positions,capital_flows,interest,collateral_usd,updated_at) "
        "VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,NOW()) "
        "ON CONFLICT(date) DO UPDATE SET cash=EXCLUDED.cash,positions_value=EXCLUDED.positions_value,"
        "equity=EXCLUDED.equity,realized_gross_pnl=EXCLUDED.realized_gross_pnl,fees=EXCLUDED.fees,"
        "funding=EXCLUDED.funding,open_positions=EXCLUDED.open_positions,capital_flows=EXCLUDED.capital_flows,"
        "interest=EXCLUDED.interest,collateral_usd=EXCLUDED.collateral_usd,updated_at=NOW();",
        static_cast<int>(values.size()), values.data()));
}


std::set<Timestamp> PostgresStateStore::equityDays() const
{
    PGresult* result = exec(impl_->connection, "SELECT date FROM trading_equity_daily;", PGRES_TUPLES_OK);
    std::set<Timestamp> days;
    for (int row = 0; row < PQntuples(result); ++row)
        days.insert(static_cast<Timestamp>(std::strtoul(PQgetvalue(result, row, 0), nullptr, 10)));
    clear(result);
    return days;
}


bool PostgresStateStore::knowsVenueCost(const std::string& costId) const
{
    const std::array<const char*, 1> values{costId.c_str()};
    PGresult* result = execParams(impl_->connection, "SELECT 1 FROM trading_venue_costs WHERE cost_id=$1;",
                                  1, values.data(), PGRES_TUPLES_OK);
    const bool known = PQntuples(result) > 0;
    clear(result);
    return known;
}


void PostgresStateStore::saveVenueCost(const TradingStateSnapshot& snapshot, const VenueCostEvent& cost)
{
    snapshot.requireMarketOnly();
    const std::string snapshotJson = encodeSnapshot(snapshot).dump();
    const std::string schemaVersion = std::to_string(snapshot.schema_version);

    inTransaction(impl_->connection, [&] {
        const std::array<std::string, 6> parameters{
            cost.cost_id, venueCostKindName(cost.kind), std::to_string(cost.timestamp), cost.coin,
            std::to_string(cost.fill_id), encodeDoubleForPostgres(cost.amount_usd)
        };
        std::array<const char*, 6> values{};
        for (std::size_t i = 0; i < parameters.size(); ++i)
            values[i] = parameters[i].c_str();
        PGresult* inserted = execParams(
            impl_->connection,
            "INSERT INTO trading_venue_costs(cost_id,kind,timestamp,coin,fill_id,amount) "
            "VALUES($1,$2,$3,$4,$5,$6) ON CONFLICT(cost_id) DO NOTHING;",
            static_cast<int>(values.size()), values.data());
        const bool isNew = std::string(PQcmdTuples(inserted)) == "1";
        clear(inserted);
        if (!isNew)
            throw std::logic_error("Venue cost " + cost.cost_id + " was already applied");

        // Funding also feeds the position ledger (trading_funding).
        if (cost.kind == VenueCostKind::Funding) {
            const std::array<const char*, 4> funding{values[0], values[2], values[3], values[5]};
            clear(execParams(
                impl_->connection,
                "INSERT INTO trading_funding(payment_id,timestamp,coin,amount) VALUES($1,$2,$3,$4) "
                "ON CONFLICT(payment_id) DO NOTHING;",
                static_cast<int>(funding.size()), funding.data()));
        }

        const std::array<const char*, 2> state{schemaVersion.c_str(), snapshotJson.c_str()};
        clear(execParams(
            impl_->connection,
            "INSERT INTO trading_runtime_state(singleton,schema_version,snapshot,updated_at) "
            "VALUES(TRUE,$1,$2::jsonb,NOW()) "
            "ON CONFLICT(singleton) DO UPDATE SET "
            "schema_version=EXCLUDED.schema_version,snapshot=EXCLUDED.snapshot,updated_at=NOW();",
            static_cast<int>(state.size()), state.data()));
    });
}


std::unordered_map<FillID, double> PostgresStateStore::loadFillFees() const
{
    PGresult* result = exec(
        impl_->connection,
        "SELECT fill_id,SUM(-amount) FROM trading_venue_costs WHERE kind='FEE' AND fill_id<>0 GROUP BY fill_id;",
        PGRES_TUPLES_OK);
    std::unordered_map<FillID, double> fees;
    for (int row = 0; row < PQntuples(result); ++row)
        fees[std::strtoull(PQgetvalue(result, row, 0), nullptr, 10)] = std::strtod(PQgetvalue(result, row, 1), nullptr);
    clear(result);
    return fees;
}


std::vector<VenueCostEvent> PostgresStateStore::loadAccountCashMovements() const
{
    PGresult* result = exec(
        impl_->connection,
        "SELECT cost_id,kind,timestamp,amount FROM trading_venue_costs "
        "WHERE kind IN ('INTEREST','CAPITAL_FLOW') ORDER BY timestamp,cost_id;",
        PGRES_TUPLES_OK);
    std::vector<VenueCostEvent> movements;
    for (int row = 0; row < PQntuples(result); ++row) {
        VenueCostEvent movement;
        movement.cost_id = PQgetvalue(result, row, 0);
        movement.kind = venueCostKindFromName(PQgetvalue(result, row, 1)).value();
        movement.timestamp = static_cast<Timestamp>(std::strtoul(PQgetvalue(result, row, 2), nullptr, 10));
        movement.amount_usd = std::strtod(PQgetvalue(result, row, 3), nullptr);
        movements.push_back(std::move(movement));
    }
    clear(result);
    return movements;
}


double PostgresStateStore::fixDailyCollateral(Timestamp day, double current)
{
    const std::string date = std::to_string(day);
    const std::string amount = encodeDoubleForPostgres(current);
    const std::array<const char*, 2> values{date.c_str(), amount.c_str()};
    clear(execParams(
        impl_->connection,
        "INSERT INTO trading_daily_collateral(date,collateral_usd) VALUES($1,$2) ON CONFLICT(date) DO NOTHING;",
        static_cast<int>(values.size()), values.data()));
    return dailyCollateral(day).value();
}


std::optional<double> PostgresStateStore::dailyCollateral(Timestamp day) const
{
    const std::string date = std::to_string(day);
    const std::array<const char*, 1> values{date.c_str()};
    PGresult* result = execParams(impl_->connection,
                                  "SELECT collateral_usd FROM trading_daily_collateral WHERE date=$1;",
                                  1, values.data(), PGRES_TUPLES_OK);
    std::optional<double> stored;
    if (PQntuples(result) > 0)
        stored = std::strtod(PQgetvalue(result, 0, 0), nullptr);
    clear(result);
    return stored;
}


std::vector<FundingPayment> PostgresStateStore::loadFunding() const
{
    PGresult* result = exec(
        impl_->connection,
        "SELECT payment_id,timestamp,coin,amount FROM trading_funding ORDER BY timestamp,payment_id;",
        PGRES_TUPLES_OK);
    std::vector<FundingPayment> payments;
    for (int row = 0; row < PQntuples(result); ++row) {
        FundingPayment payment;
        payment.payment_id = PQgetvalue(result, row, 0);
        payment.timestamp = static_cast<Timestamp>(std::strtoul(PQgetvalue(result, row, 1), nullptr, 10));
        payment.coin = PQgetvalue(result, row, 2);
        payment.amount = std::strtod(PQgetvalue(result, row, 3), nullptr);
        payments.push_back(std::move(payment));
    }
    clear(result);
    return payments;
}
