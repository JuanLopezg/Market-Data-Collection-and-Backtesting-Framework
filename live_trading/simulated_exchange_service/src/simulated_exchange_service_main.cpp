// Implements the durable simulated exchange backend used by the gateway and historical/system
// tests.
//
// 1. Recover exchange cash, positions, orders, fills, and durable outbox state.
// 2. Consume execution prices plus submit/cancel/snapshot backend commands.
// 3. Apply exchange semantics, checkpoint first, then publish order/fill/snapshot events from
// the durable outbox.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <iomanip>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <libpq-fe.h>

#include "account.h"
#include "canonical_market_data_reader.h"
#include "message_json.h"
#include "exchange_snapshot_event.h"
#include "exchange_snapshot_request.h"
#include "execution_messages.h"
#include "jetstream_bus.h"
#include "service_logging.h"
#include "adapters/simulated_exchange.h"
#include "time_handler_factory.h"
#include "time_utils.h"
#include "message_subjects.h"


// Internal helpers and service implementation.

namespace {

std::atomic<bool> running{true};

void stopHandler(int)
{
    running.store(false);
}

bool envFlag(const char* name)
{
    const char* value = std::getenv(name);
    if (value == nullptr)
        return false;
    const std::string text(value);
    return !text.empty() && text != "0" && text != "false" && text != "FALSE";
}

double envDouble(const char* name, double fallback)
{
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0')
        return fallback;
    return std::stod(value);
}

Timestamp currentBusinessUtcDate(const TimeHandler& timeHandler)
{
    return static_cast<Timestamp>(
        toYYYYMMDD(getCurrentUtcDate(timeHandler.getTime()))
    );
}


// Command-line configuration.

struct Options {
    std::string nats_url = "nats://127.0.0.1:4222";
    std::string runtime_stream = "ALGOTRADING_RUNTIME";
    std::string backend_stream = "ALGOTRADING_EXCHANGE_BACKEND";
    std::string postgres;
    std::string market_data_db; // Optional read-only OHLCV source for PAPER position stops.
    double initial_cash = 100000.0;
    double commission_rate = 0.0;
    int poll_timeout_ms = 250;
};


Options parseOptions(int argc, char** argv)
{
    Options options;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto requireValue = [&](const char* option) -> std::string {
            if (i + 1 >= argc)
                throw std::invalid_argument(std::string("Missing value for ") + option);
            return argv[++i];
        };

        if (arg == "--nats-url")
            options.nats_url = requireValue("--nats-url");
        else if (arg == "--stream")
            options.runtime_stream = requireValue("--stream");
        else if (arg == "--backend-stream")
            options.backend_stream = requireValue("--backend-stream");
        else if (arg == "--postgres")
            options.postgres = requireValue("--postgres");
        else if (arg == "--market-data-db")
            options.market_data_db = requireValue("--market-data-db");
        else if (arg == "--initial-cash")
            options.initial_cash = std::stod(requireValue("--initial-cash"));
        else if (arg == "--commission-rate")
            options.commission_rate = std::stod(requireValue("--commission-rate"));
        else if (arg == "--poll-timeout-ms")
            options.poll_timeout_ms = std::stoi(requireValue("--poll-timeout-ms"));
        else if (arg == "--help" || arg == "-h") {
            std::cout
                << "Simulated exchange service options:\n"
                << "  --nats-url URL\n"
                << "  --stream NAME\n"
                << "  --backend-stream NAME\n"
                << "  --postgres CONNECTION_STRING\n"
                << "  --market-data-db PATH    read closed daily bars after market.data.updated\n"
                << "  --initial-cash VALUE\n"
                << "  --commission-rate VALUE\n"
                << "  --poll-timeout-ms N\n";
            std::exit(0);
        }
        else
            throw std::invalid_argument("Unknown option: " + arg);
    }

    if (options.runtime_stream.empty() || options.backend_stream.empty())
        throw std::invalid_argument("Simulated-exchange stream names cannot be empty");
    if (!std::isfinite(options.initial_cash) || options.initial_cash <= 0.0)
        throw std::invalid_argument("--initial-cash must be finite and positive");
    if (!std::isfinite(options.commission_rate) || options.commission_rate < 0.0)
        throw std::invalid_argument("--commission-rate must be finite and non-negative");
    if (options.poll_timeout_ms <= 0)
        throw std::invalid_argument("--poll-timeout-ms must be positive");

    return options;
}


bool sameOrder(const ExecutionOrder& lhs, const ExecutionOrder& rhs)
{
    return lhs.order_id == rhs.order_id &&
           lhs.strategy_id == rhs.strategy_id &&
           lhs.created_at == rhs.created_at &&
           lhs.active_from == rhs.active_from &&
           lhs.coin == rhs.coin &&
           lhs.side == rhs.side &&
           lhs.quantity == rhs.quantity &&
           lhs.position_stop_price == rhs.position_stop_price;
}


bool samePrices(
    const std::unordered_map<Coin, double>& lhs,
    const std::unordered_map<Coin, double>& rhs
)
{
    return lhs == rhs;
}


CoinBarMap openBars(const std::unordered_map<Coin, double>& prices)
{
    CoinBarMap bars;
    bars.reserve(prices.size());

    for (const auto& [coin, price] : prices) {
        if (!std::isfinite(price) || price <= 0.0)
            throw std::invalid_argument("Simulated execution price must be finite and positive");

        BarData bar;
        bar.open = price;
        bars.emplace(coin, bar);
    }

    return bars;
}


std::string orderUpdateMessageId(const OrderUpdate& update)
{
    return "sim-order-update:" + std::to_string(update.order_id) + ":" +
           std::to_string(static_cast<int>(update.status)) + ":" +
           std::to_string(update.timestamp);
}


std::string fillMessageId(const Fill& fill)
{
    return "sim-fill:" + std::to_string(fill.fill_id);
}


std::string doubleText(double value)
{
    std::ostringstream output;
    output << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return output.str();
}


// Durable PostgreSQL persistence helpers.

class PgResult {
private:
    PGresult* result_ = nullptr;

public:
    explicit PgResult(PGresult* result) : result_(result) {}

    ~PgResult()
    {
        if (result_)
            PQclear(result_);
    }

    PgResult(const PgResult&) = delete;
    PgResult& operator=(const PgResult&) = delete;

    PgResult(PgResult&& other) noexcept
        : result_(std::exchange(other.result_, nullptr))
    {}

    PgResult& operator=(PgResult&& other) noexcept
    {
        if (this != &other) {
            if (result_)
                PQclear(result_);
            result_ = std::exchange(other.result_, nullptr);
        }
        return *this;
    }

    PGresult* get() const { return result_; }
};


struct DurableOutboxRecord {
    long long sequence = 0;
    std::string subject;
    std::string message_id;
    std::string payload;
};


class SimulatedExchangeCheckpointStore {
public:
    struct RecoveryState {
        double cash = 0.0;
        Timestamp latest_timestamp = 0;
        Timestamp last_position_stop_bar = 0;
        FillID next_fill_id = 1;
        std::unordered_map<Coin, double> positions;
        std::map<Timestamp, std::unordered_map<Coin, double>> execution_prices;
        std::unordered_map<OrderID, ExecutionOrder> known_orders;
        std::unordered_map<OrderID, ExecutionOrder> active_orders;
        std::size_t unpublished_outbox = 0;

        bool nonEmpty() const
        {
            return latest_timestamp != 0 ||
                   next_fill_id != 1 ||
                   !positions.empty() ||
                   !execution_prices.empty() ||
                   !known_orders.empty() ||
                   !active_orders.empty() ||
                   unpublished_outbox != 0;
        }
    };

private:
    PGconn* connection_ = nullptr;
    static constexpr const char* STATE_KEY = "simulated-exchange";

    void requireConnection() const
    {
        if (connection_ == nullptr || PQstatus(connection_) != CONNECTION_OK)
            throw std::runtime_error("Simulated-exchange checkpoint PostgreSQL connection is not ready");
    }

    PgResult exec(const std::string& sql, ExecStatusType expected) const
    {
        requireConnection();
        PgResult result(PQexec(connection_, sql.c_str()));
        if (result.get() == nullptr || PQresultStatus(result.get()) != expected) {
            const std::string error = connection_ ? PQerrorMessage(connection_) : "unknown PostgreSQL error";
            throw std::runtime_error("Simulated-exchange checkpoint PostgreSQL query failed: " + error);
        }
        return result;
    }

    PgResult execParams(
        const std::string& sql,
        const std::vector<std::string>& parameters,
        ExecStatusType expected
    ) const
    {
        requireConnection();

        std::vector<const char*> values;
        values.reserve(parameters.size());
        for (const std::string& parameter : parameters)
            values.push_back(parameter.c_str());

        PgResult result(PQexecParams(
            connection_,
            sql.c_str(),
            static_cast<int>(values.size()),
            nullptr,
            values.data(),
            nullptr,
            nullptr,
            0
        ));

        if (result.get() == nullptr || PQresultStatus(result.get()) != expected) {
            const std::string error = connection_ ? PQerrorMessage(connection_) : "unknown PostgreSQL error";
            throw std::runtime_error(
                "Simulated-exchange checkpoint PostgreSQL parameterized query failed: " + error
            );
        }
        return result;
    }

    void ensureSchema(double initialCash, double commissionRate)
    {
        exec(
            "CREATE TABLE IF NOT EXISTS simulated_exchange_metadata ("
            "state_key TEXT PRIMARY KEY, "
            "initial_cash DOUBLE PRECISION NOT NULL, "
            "commission_rate DOUBLE PRECISION NOT NULL"
            ")",
            PGRES_COMMAND_OK
        );

        exec(
            "CREATE TABLE IF NOT EXISTS simulated_exchange_state ("
            "state_key TEXT PRIMARY KEY, "
            "cash DOUBLE PRECISION NOT NULL, "
            "latest_timestamp BIGINT NOT NULL, "
            "next_fill_id NUMERIC(20,0) NOT NULL"
            ")",
            PGRES_COMMAND_OK
        );

        exec(
            "CREATE TABLE IF NOT EXISTS simulated_exchange_positions ("
            "state_key TEXT NOT NULL, "
            "coin TEXT NOT NULL, "
            "quantity DOUBLE PRECISION NOT NULL, "
            "PRIMARY KEY(state_key, coin)"
            ")",
            PGRES_COMMAND_OK
        );

        exec(
            "CREATE TABLE IF NOT EXISTS simulated_exchange_execution_prices ("
            "state_key TEXT NOT NULL, "
            "timestamp BIGINT NOT NULL, "
            "coin TEXT NOT NULL, "
            "price DOUBLE PRECISION NOT NULL, "
            "PRIMARY KEY(state_key, timestamp, coin)"
            ")",
            PGRES_COMMAND_OK
        );

        exec(
            "CREATE TABLE IF NOT EXISTS simulated_exchange_known_orders ("
            "state_key TEXT NOT NULL, "
            "order_id BIGINT NOT NULL, "
            "strategy_id BIGINT NOT NULL, "
            "created_at BIGINT NOT NULL, "
            "active_from BIGINT NOT NULL, "
            "coin TEXT NOT NULL, "
            "side SMALLINT NOT NULL, "
            "quantity DOUBLE PRECISION NOT NULL, "
            "PRIMARY KEY(state_key, order_id)"
            ")",
            PGRES_COMMAND_OK
        );

        exec(
            "CREATE TABLE IF NOT EXISTS simulated_exchange_active_orders ("
            "state_key TEXT NOT NULL, "
            "order_id BIGINT NOT NULL, "
            "PRIMARY KEY(state_key, order_id)"
            ")",
            PGRES_COMMAND_OK
        );
        exec("ALTER TABLE simulated_exchange_known_orders ADD COLUMN IF NOT EXISTS "
             "position_stop_price DOUBLE PRECISION NOT NULL DEFAULT 0", PGRES_COMMAND_OK);
        exec("ALTER TABLE simulated_exchange_state ADD COLUMN IF NOT EXISTS "
             "last_position_stop_bar BIGINT NOT NULL DEFAULT 0", PGRES_COMMAND_OK);

        exec(
            "CREATE TABLE IF NOT EXISTS simulated_exchange_outbox ("
            "sequence BIGSERIAL PRIMARY KEY, "
            "state_key TEXT NOT NULL, "
            "subject TEXT NOT NULL, "
            "message_id TEXT NOT NULL, "
            "payload TEXT NOT NULL, "
            "published BOOLEAN NOT NULL DEFAULT FALSE, "
            "UNIQUE(state_key, message_id)"
            ")",
            PGRES_COMMAND_OK
        );

        execParams(
            "INSERT INTO simulated_exchange_metadata(state_key, initial_cash, commission_rate) "
            "VALUES($1, $2, $3) ON CONFLICT(state_key) DO NOTHING",
            {STATE_KEY, doubleText(initialCash), doubleText(commissionRate)},
            PGRES_COMMAND_OK
        );

        const PgResult metadata = execParams(
            "SELECT initial_cash, commission_rate FROM simulated_exchange_metadata "
            "WHERE state_key = $1",
            {STATE_KEY},
            PGRES_TUPLES_OK
        );

        if (PQntuples(metadata.get()) != 1)
            throw std::runtime_error("Simulated-exchange checkpoint metadata row is missing");

        const double persistedInitialCash = std::stod(PQgetvalue(metadata.get(), 0, 0));
        const double persistedCommissionRate = std::stod(PQgetvalue(metadata.get(), 0, 1));
        if (persistedInitialCash != initialCash || persistedCommissionRate != commissionRate)
            throw std::runtime_error(
                "Persisted simulated-exchange checkpoint uses different cash/commission configuration"
            );

        execParams(
            "INSERT INTO simulated_exchange_state(state_key, cash, latest_timestamp, next_fill_id) "
            "VALUES($1, $2, 0, 1) ON CONFLICT(state_key) DO NOTHING",
            {STATE_KEY, doubleText(initialCash)},
            PGRES_COMMAND_OK
        );
    }

    static ExecutionOrder decodeOrder(PGresult* result, int row, int columnOffset)
    {
        const unsigned long long orderIdValue =
            std::stoull(PQgetvalue(result, row, columnOffset + 0));
        const unsigned long long strategyIdValue =
            std::stoull(PQgetvalue(result, row, columnOffset + 1));
        const unsigned long long createdAtValue =
            std::stoull(PQgetvalue(result, row, columnOffset + 2));
        const unsigned long long activeFromValue =
            std::stoull(PQgetvalue(result, row, columnOffset + 3));

        if (orderIdValue > std::numeric_limits<OrderID>::max() ||
            strategyIdValue > std::numeric_limits<StrategyID>::max() ||
            createdAtValue > std::numeric_limits<Timestamp>::max() ||
            activeFromValue > std::numeric_limits<Timestamp>::max())
            throw std::runtime_error("Persisted simulated-exchange order identifier is out of range");

        const int sideValue = std::stoi(PQgetvalue(result, row, columnOffset + 5));
        if (sideValue != 0 && sideValue != 1)
            throw std::runtime_error("Persisted simulated-exchange order side is invalid");

        ExecutionOrder order(
            static_cast<OrderID>(orderIdValue),
            static_cast<StrategyID>(strategyIdValue),
            static_cast<Timestamp>(createdAtValue),
            static_cast<Timestamp>(activeFromValue),
            PQgetvalue(result, row, columnOffset + 4),
            sideValue == 0 ? OrderSide::Buy : OrderSide::Sell,
            std::stod(PQgetvalue(result, row, columnOffset + 6))
        );
        order.position_stop_price = std::stod(PQgetvalue(result, row, columnOffset + 7));
        order.validateConditional();
        return order;
    }

public:
    SimulatedExchangeCheckpointStore(
        const std::string& connectionString,
        double initialCash,
        double commissionRate
    )
    {
        connection_ = PQconnectdb(connectionString.c_str());
        if (connection_ == nullptr || PQstatus(connection_) != CONNECTION_OK) {
            const std::string error = connection_ ? PQerrorMessage(connection_) : "cannot allocate PGconn";
            if (connection_) {
                PQfinish(connection_);
                connection_ = nullptr;
            }
            throw std::runtime_error(
                "Cannot connect simulated-exchange checkpoint store to PostgreSQL: " + error
            );
        }

        ensureSchema(initialCash, commissionRate);
    }

    ~SimulatedExchangeCheckpointStore()
    {
        if (connection_)
            PQfinish(connection_);
    }

    SimulatedExchangeCheckpointStore(const SimulatedExchangeCheckpointStore&) = delete;
    SimulatedExchangeCheckpointStore& operator=(const SimulatedExchangeCheckpointStore&) = delete;

    RecoveryState load() const
    {
        RecoveryState state;

        const PgResult stateRow = execParams(
            "SELECT cash, latest_timestamp, next_fill_id, last_position_stop_bar FROM simulated_exchange_state "
            "WHERE state_key = $1",
            {STATE_KEY},
            PGRES_TUPLES_OK
        );
        if (PQntuples(stateRow.get()) != 1)
            throw std::runtime_error("Simulated-exchange durable state row is missing");

        state.cash = std::stod(PQgetvalue(stateRow.get(), 0, 0));

        const unsigned long long latestTimestampValue =
            std::stoull(PQgetvalue(stateRow.get(), 0, 1));
        if (latestTimestampValue > std::numeric_limits<Timestamp>::max())
            throw std::runtime_error("Persisted simulated-exchange timestamp is out of range");
        state.latest_timestamp = static_cast<Timestamp>(latestTimestampValue);
        const auto lastBar = std::stoull(PQgetvalue(stateRow.get(), 0, 3));
        if (lastBar > std::numeric_limits<Timestamp>::max())
            throw std::runtime_error("Persisted position stop bar timestamp is out of range");
        state.last_position_stop_bar = static_cast<Timestamp>(lastBar);

        state.next_fill_id = static_cast<FillID>(
            std::stoull(PQgetvalue(stateRow.get(), 0, 2))
        );
        if (state.next_fill_id == 0)
            throw std::runtime_error("Persisted simulated-exchange next FillID is invalid");

        const PgResult positions = execParams(
            "SELECT coin, quantity FROM simulated_exchange_positions "
            "WHERE state_key = $1 ORDER BY coin",
            {STATE_KEY},
            PGRES_TUPLES_OK
        );
        for (int row = 0; row < PQntuples(positions.get()); ++row)
            state.positions.emplace(
                PQgetvalue(positions.get(), row, 0),
                std::stod(PQgetvalue(positions.get(), row, 1))
            );

        const PgResult prices = execParams(
            "SELECT timestamp, coin, price FROM simulated_exchange_execution_prices "
            "WHERE state_key = $1 ORDER BY timestamp, coin",
            {STATE_KEY},
            PGRES_TUPLES_OK
        );
        for (int row = 0; row < PQntuples(prices.get()); ++row) {
            const unsigned long long timestampValue =
                std::stoull(PQgetvalue(prices.get(), row, 0));
            if (timestampValue > std::numeric_limits<Timestamp>::max())
                throw std::runtime_error("Persisted simulated execution-price timestamp is out of range");

            state.execution_prices[static_cast<Timestamp>(timestampValue)].emplace(
                PQgetvalue(prices.get(), row, 1),
                std::stod(PQgetvalue(prices.get(), row, 2))
            );
        }

        const PgResult knownOrders = execParams(
            "SELECT order_id, strategy_id, created_at, active_from, coin, side, quantity, position_stop_price "
            "FROM simulated_exchange_known_orders WHERE state_key = $1 ORDER BY order_id",
            {STATE_KEY},
            PGRES_TUPLES_OK
        );
        for (int row = 0; row < PQntuples(knownOrders.get()); ++row) {
            ExecutionOrder order = decodeOrder(knownOrders.get(), row, 0);
            state.known_orders.emplace(order.order_id, std::move(order));
        }

        const PgResult activeOrders = execParams(
            "SELECT order_id FROM simulated_exchange_active_orders "
            "WHERE state_key = $1 ORDER BY order_id",
            {STATE_KEY},
            PGRES_TUPLES_OK
        );
        for (int row = 0; row < PQntuples(activeOrders.get()); ++row) {
            const unsigned long long orderIdValue =
                std::stoull(PQgetvalue(activeOrders.get(), row, 0));
            if (orderIdValue > std::numeric_limits<OrderID>::max())
                throw std::runtime_error("Persisted active simulated order id is out of range");

            const OrderID orderId = static_cast<OrderID>(orderIdValue);
            const auto known = state.known_orders.find(orderId);
            if (known == state.known_orders.end())
                throw std::runtime_error("Persisted active simulated order has no known-order row");
            state.active_orders.emplace(orderId, known->second);
        }

        const PgResult outboxCount = execParams(
            "SELECT count(*) FROM simulated_exchange_outbox "
            "WHERE state_key = $1 AND published = FALSE",
            {STATE_KEY},
            PGRES_TUPLES_OK
        );
        state.unpublished_outbox = static_cast<std::size_t>(
            std::stoull(PQgetvalue(outboxCount.get(), 0, 0))
        );

        return state;
    }

    void saveTransition(
        double cash,
        const std::unordered_map<Coin, double>& positions,
        Timestamp latestTimestamp,
        Timestamp lastPositionStopBar,
        FillID nextFillId,
        const std::unordered_map<OrderID, ExecutionOrder>& activeOrders,
        const std::optional<std::pair<Timestamp, std::unordered_map<Coin, double>>>& newPrices,
        const std::optional<ExecutionOrder>& newKnownOrder,
        const std::vector<DurableOutboxRecord>& outbox
    )
    {
        exec("BEGIN", PGRES_COMMAND_OK);
        try {
            execParams(
                "UPDATE simulated_exchange_state "
                "SET cash = $2, latest_timestamp = $3, next_fill_id = $4, last_position_stop_bar = $5 "
                "WHERE state_key = $1",
                {
                    STATE_KEY,
                    doubleText(cash),
                    std::to_string(latestTimestamp),
                    std::to_string(nextFillId),
                    std::to_string(lastPositionStopBar)
                },
                PGRES_COMMAND_OK
            );

            execParams(
                "DELETE FROM simulated_exchange_positions WHERE state_key = $1",
                {STATE_KEY},
                PGRES_COMMAND_OK
            );
            for (const auto& [coin, quantity] : positions) {
                execParams(
                    "INSERT INTO simulated_exchange_positions(state_key, coin, quantity) "
                    "VALUES($1, $2, $3)",
                    {STATE_KEY, coin, doubleText(quantity)},
                    PGRES_COMMAND_OK
                );
            }

            if (newPrices.has_value()) {
                for (const auto& [coin, price] : newPrices->second) {
                    execParams(
                        "INSERT INTO simulated_exchange_execution_prices("
                        "state_key, timestamp, coin, price"
                        ") VALUES($1, $2, $3, $4) ON CONFLICT DO NOTHING",
                        {
                            STATE_KEY,
                            std::to_string(newPrices->first),
                            coin,
                            doubleText(price)
                        },
                        PGRES_COMMAND_OK
                    );
                }
            }

            if (newKnownOrder.has_value()) {
                const ExecutionOrder& order = *newKnownOrder;
                execParams(
                    "INSERT INTO simulated_exchange_known_orders("
                    "state_key, order_id, strategy_id, created_at, active_from, coin, side, quantity, position_stop_price"
                    ") VALUES($1, $2, $3, $4, $5, $6, $7, $8, $9) "
                    "ON CONFLICT(state_key, order_id) DO NOTHING",
                    {
                        STATE_KEY,
                        std::to_string(order.order_id),
                        std::to_string(order.strategy_id),
                        std::to_string(order.created_at),
                        std::to_string(order.active_from),
                        order.coin,
                        order.side == OrderSide::Buy ? "0" : "1",
                        doubleText(order.quantity),
                        doubleText(order.position_stop_price)
                    },
                    PGRES_COMMAND_OK
                );
            }

            execParams(
                "DELETE FROM simulated_exchange_active_orders WHERE state_key = $1",
                {STATE_KEY},
                PGRES_COMMAND_OK
            );
            for (const auto& [orderId, order] : activeOrders) {
                (void)order;
                execParams(
                    "INSERT INTO simulated_exchange_active_orders(state_key, order_id) "
                    "VALUES($1, $2)",
                    {STATE_KEY, std::to_string(orderId)},
                    PGRES_COMMAND_OK
                );
            }

            for (const DurableOutboxRecord& event : outbox) {
                execParams(
                    "INSERT INTO simulated_exchange_outbox("
                    "state_key, subject, message_id, payload, published"
                    ") VALUES($1, $2, $3, $4, FALSE) "
                    "ON CONFLICT(state_key, message_id) DO NOTHING",
                    {STATE_KEY, event.subject, event.message_id, event.payload},
                    PGRES_COMMAND_OK
                );
            }

            exec("COMMIT", PGRES_COMMAND_OK);
        }
        catch (...) {
            try {
                exec("ROLLBACK", PGRES_COMMAND_OK);
            }
            catch (...) {
            }
            throw;
        }
    }

    std::vector<DurableOutboxRecord> unpublishedOutbox() const
    {
        const PgResult result = execParams(
            "SELECT sequence, subject, message_id, payload FROM simulated_exchange_outbox "
            "WHERE state_key = $1 AND published = FALSE ORDER BY sequence",
            {STATE_KEY},
            PGRES_TUPLES_OK
        );

        std::vector<DurableOutboxRecord> rows;
        rows.reserve(static_cast<std::size_t>(PQntuples(result.get())));
        for (int row = 0; row < PQntuples(result.get()); ++row) {
            DurableOutboxRecord value;
            value.sequence = std::stoll(PQgetvalue(result.get(), row, 0));
            value.subject = PQgetvalue(result.get(), row, 1);
            value.message_id = PQgetvalue(result.get(), row, 2);
            value.payload = PQgetvalue(result.get(), row, 3);
            rows.push_back(std::move(value));
        }
        return rows;
    }

    void markPublished(long long sequence) const
    {
        execParams(
            "UPDATE simulated_exchange_outbox SET published = TRUE "
            "WHERE state_key = $1 AND sequence = $2",
            {STATE_KEY, std::to_string(sequence)},
            PGRES_COMMAND_OK
        );
    }
};


// Service runtime and message-processing loop.

class SimulatedExchangeServiceRuntime {
private:
    const Options options_;
    JetStreamBus bus_;
    TimeHandler time_handler_;
    SimulatedExchange exchange_;
    Account account_;
    std::unique_ptr<SimulatedExchangeCheckpointStore> checkpoint_store_;

    std::map<Timestamp, std::unordered_map<Coin, double>> execution_prices_;
    std::unordered_map<OrderID, ExecutionOrder> known_orders_;
    std::vector<ExchangeEvent> pending_events_;
    Timestamp latest_timestamp_ = 0;
    Timestamp last_position_stop_bar_ = 0;

    MessageBus::SubscriptionID prices_subscription_ = 0;
    MessageBus::SubscriptionID bars_subscription_ = 0;
    MessageBus::SubscriptionID market_update_subscription_ = 0;
    MessageBus::SubscriptionID submit_subscription_ = 0;
    MessageBus::SubscriptionID cancel_subscription_ = 0;
    MessageBus::SubscriptionID snapshot_request_subscription_ = 0;
    bool chaos_crash_after_checkpoint_once_ =
        envFlag("ALGOTRADING_CHAOS_CRASH_AFTER_CHECKPOINT_ONCE");

    DurableConsumerOptions consumer(
        const std::string& stream,
        const std::string& durable,
        const std::string& subject
    ) const
    {
        DurableConsumerOptions result;
        result.stream = stream;
        result.durable_name = durable;
        result.subject = subject;
        result.ack_wait_ms = 30000;
        result.max_deliver = 20;
        result.max_ack_pending = 256;
        return result;
    }

    static void validateMetadata(const ContractMetadata& metadata)
    {
        if (metadata.schema_version != 1 || metadata.message_id.empty())
            throw std::invalid_argument("Invalid simulated-exchange contract metadata");
    }

    bool businessTimeReady(Timestamp timestamp, const char* eventName) const
    {
        // produced_at==0 is reserved for bootstrap/control-style requests that do not
        // carry an economic timestamp yet. Preserve that existing contract convention.
        if (timestamp == 0)
            return true;

        const Timestamp businessNow = currentBusinessUtcDate(time_handler_);
        if (timestamp <= businessNow)
            return true;

        LG_DEBUG("{} for {} waits for business time (today is {}); will retry",
                 eventName, FormatDayKey(timestamp), FormatDayKey(businessNow));
        return false;
    }

    ContractMetadata eventMetadata(
        std::string messageId,
        std::string correlationId,
        Timestamp timestamp
    ) const
    {
        ContractMetadata result;
        result.schema_version = 1;
        result.message_id = std::move(messageId);
        result.correlation_id = std::move(correlationId);
        result.produced_at = timestamp;
        return result;
    }

    void captureExchangeEvents()
    {
        std::vector<ExchangeEvent> generated = exchange_.drainEvents();
        OrderID previousFillOrderId = 0;
        bool previousWasFill = false;

        for (ExchangeEvent& event : generated) {
            if (const auto* fill = std::get_if<Fill>(&event)) {
                account_.applyFill(*fill);
                if (previousWasFill && previousFillOrderId == fill->order_id) {
                    LG_WARN("Fault test: order {} was split into a partial fill; second fill {} has quantity {}",
                            fill->order_id, fill->fill_id, FormatQuantity(fill->quantity));
                }
                previousFillOrderId = fill->order_id;
                previousWasFill = true;
            }
            else {
                previousWasFill = false;
                if (const auto* update = std::get_if<OrderUpdate>(&event);
                    update != nullptr &&
                    update->status == ExecutionOrderStatus::Rejected &&
                    update->message == "CHAOS_REJECT_ONCE") {
                    LG_WARN("Fault test: injected a rejection for order {} on {}",
                            update->order_id, FormatDayKey(update->timestamp));
                }
            }
            pending_events_.push_back(std::move(event));
        }
    }

    std::vector<DurableOutboxRecord> durableOutboxRecords(
        const std::string& correlationId
    ) const
    {
        std::vector<DurableOutboxRecord> result;
        result.reserve(pending_events_.size());

        for (const ExchangeEvent& event : pending_events_) {
            DurableOutboxRecord record;

            if (const auto* update = std::get_if<OrderUpdate>(&event)) {
                OrderUpdateEvent output;
                output.metadata = eventMetadata(
                    orderUpdateMessageId(*update),
                    correlationId,
                    update->timestamp
                );
                output.update = *update;

                record.subject = MessageSubjects::BACKEND_ORDER_UPDATE;
                record.message_id = output.metadata.message_id;
                record.payload = MessageJson::encode(output);
            }
            else if (const auto* fill = std::get_if<Fill>(&event)) {
                FillEvent output;
                output.metadata = eventMetadata(
                    fillMessageId(*fill),
                    correlationId,
                    fill->timestamp
                );
                output.fill = *fill;

                record.subject = MessageSubjects::BACKEND_FILL;
                record.message_id = output.metadata.message_id;
                record.payload = MessageJson::encode(output);
            }
            else {
                throw std::logic_error("Unsupported simulated-exchange event in durable outbox");
            }

            result.push_back(std::move(record));
        }

        return result;
    }

    void flushDurableOutbox()
    {
        if (!checkpoint_store_)
            return;

        const std::vector<DurableOutboxRecord> rows = checkpoint_store_->unpublishedOutbox();
        for (const DurableOutboxRecord& row : rows) {
            bus_.publish(row.subject, row.payload, row.message_id);
            checkpoint_store_->markPublished(row.sequence);
            LG_DEBUG("Published saved outbox event {} to {} (message id {})",
                     row.sequence, row.subject, row.message_id);
        }
    }

    void flushPendingEvents(const std::string& correlationId)
    {
        if (checkpoint_store_) {
            if (!pending_events_.empty())
                throw std::logic_error(
                    "Cannot publish simulated-exchange events before durable checkpoint"
                );
            flushDurableOutbox();
            return;
        }

        std::size_t published = 0;

        while (published < pending_events_.size()) {
            const ExchangeEvent& event = pending_events_[published];

            if (const auto* update = std::get_if<OrderUpdate>(&event)) {
                OrderUpdateEvent output;
                output.metadata = eventMetadata(
                    orderUpdateMessageId(*update),
                    correlationId,
                    update->timestamp
                );
                output.update = *update;
                bus_.publish(
                    MessageSubjects::BACKEND_ORDER_UPDATE,
                    MessageJson::encode(output),
                    output.metadata.message_id
                );
                LG_INFO("Order {} is now {} ({})",
                        output.update.order_id,
                        executionOrderStatusName(output.update.status),
                        FormatDayKey(output.update.timestamp));
                LG_DEBUG("Order update message id {}, correlation id {}",
                         output.metadata.message_id, output.metadata.correlation_id);
            }
            else if (const auto* fill = std::get_if<Fill>(&event)) {
                FillEvent output;
                output.metadata = eventMetadata(
                    fillMessageId(*fill),
                    correlationId,
                    fill->timestamp
                );
                output.fill = *fill;
                bus_.publish(
                    MessageSubjects::BACKEND_FILL,
                    MessageJson::encode(output),
                    output.metadata.message_id
                );
                LG_INFO("Filled order {}: {} {} {} at {} (commission {})",
                        output.fill.order_id,
                        output.fill.side == OrderSide::Buy ? "BUY" : "SELL",
                        FormatQuantity(output.fill.quantity),
                        output.fill.coin,
                        FormatQuantity(output.fill.price),
                        FormatQuantity(output.fill.commission));
                LG_DEBUG("Fill id {}, message id {}, correlation id {}",
                         output.fill.fill_id, output.metadata.message_id, output.metadata.correlation_id);
            }

            ++published;
        }

        if (published != 0)
            pending_events_.erase(pending_events_.begin(), pending_events_.begin() + published);
    }

    bool shouldCrashAfterCheckpoint()
    {
        if (!chaos_crash_after_checkpoint_once_)
            return false;

        constexpr const char* marker = "/tmp/algotrading_chaos_crash_after_checkpoint.done";
        if (std::ifstream(marker).good())
            return false;

        std::ofstream output(marker);
        output << "done\n";
        output.flush();
        return true;
    }

    void checkpointTransition(
        const std::string& correlationId,
        const std::optional<std::pair<Timestamp, std::unordered_map<Coin, double>>>& newPrices =
            std::nullopt,
        const std::optional<ExecutionOrder>& newKnownOrder = std::nullopt
    )
    {
        if (!checkpoint_store_) {
            flushPendingEvents(correlationId);
            return;
        }

        const std::vector<DurableOutboxRecord> outbox =
            durableOutboxRecords(correlationId);

        // Commit account/order changes and their outgoing events together. A restart
        // republishes the stored outbox instead of applying the economic transition again.
        checkpoint_store_->saveTransition(
            account_.cash(),
            account_.positions().values(),
            latest_timestamp_,
            last_position_stop_bar_,
            exchange_.nextFillId(),
            exchange_.activeOrders(),
            newPrices,
            newKnownOrder,
            outbox
        );

        const std::size_t eventCount = pending_events_.size();
        pending_events_.clear();

        LG_DEBUG("State saved for {}: cash {}, {} positions, {} active orders, {} known orders, next fill id {}, {} outbox events",
                 FormatDayKey(latest_timestamp_),
                 FormatUsd(account_.cash()),
                 account_.positions().values().size(),
                 exchange_.activeOrders().size(),
                 known_orders_.size(),
                 exchange_.nextFillId(),
                 eventCount);

        if (eventCount != 0 && shouldCrashAfterCheckpoint()) {
            LG_ALERT("Fault test: exiting with code 86 right after saving state ({} outbox events pending)", eventCount);
            std::_Exit(86);
        }

        flushDurableOutbox();
    }

    void processOpenIfAvailable(const ExecutionOrder& order)
    {
        if (order.isPositionStop())
            return; // A cached open is not a completed bar and must not trigger a stop.
        const auto priceIt = execution_prices_.find(order.active_from);
        if (priceIt == execution_prices_.end())
            return;

        exchange_.processOpen(order.active_from, openBars(priceIt->second));
        captureExchangeEvents();
    }

    void recoverDurableState()
    {
        if (options_.postgres.empty()) {
            LG_WARN("Restart recovery is disabled because PostgreSQL is not configured");
            return;
        }

        checkpoint_store_ = std::make_unique<SimulatedExchangeCheckpointStore>(
            options_.postgres,
            options_.initial_cash,
            options_.commission_rate
        );

        SimulatedExchangeCheckpointStore::RecoveryState recovered =
            checkpoint_store_->load();
        const bool recoveredNonEmpty = recovered.nonEmpty();

        account_.restoreState(recovered.cash, recovered.positions);
        exchange_.restoreState(
            std::move(recovered.active_orders),
            recovered.next_fill_id
        );
        execution_prices_ = std::move(recovered.execution_prices);
        known_orders_ = std::move(recovered.known_orders);
        latest_timestamp_ = recovered.latest_timestamp;
        last_position_stop_bar_ = recovered.last_position_stop_bar;

        LG_INFO("Recovery {}: last day {}, cash {}, {} positions, {} active orders, {} unpublished outbox events",
                recoveredNonEmpty ? "restored saved state" : "found no saved state",
                FormatDayKey(latest_timestamp_),
                FormatUsd(account_.cash()),
                account_.positions().values().size(),
                exchange_.activeOrders().size(),
                recovered.unpublished_outbox);
        LG_DEBUG("Recovered {} execution price snapshots, {} known orders, next fill id {}",
                 execution_prices_.size(), known_orders_.size(), exchange_.nextFillId());

        // If the previous process committed exchange truth but died before publishing
        // its resulting backend events, finish that durable outbox before accepting
        // new commands. Message IDs are deterministic, so a publish/mark crash can be
        // safely retried through JetStream deduplication and downstream FillID guards.
        flushDurableOutbox();
    }

    ExchangeSnapshot snapshot() const
    {
        ExchangeSnapshot result;
        result.timestamp = std::max<Timestamp>(1, latest_timestamp_);
        result.cash = account_.cash();
        result.positions = account_.positions().values();

        result.open_orders.reserve(exchange_.activeOrders().size());
        for (const auto& [orderId, order] : exchange_.activeOrders()) {
            ExchangeOpenOrderSnapshot open;
            open.local_order_id = orderId;
            open.exchange_order_id = "sim-" + std::to_string(orderId);
            open.coin = order.coin;
            open.side = order.side;
            open.quantity = order.quantity;
            open.filled_quantity = 0.0;
            result.open_orders.push_back(std::move(open));
        }

        std::sort(
            result.open_orders.begin(),
            result.open_orders.end(),
            [](const ExchangeOpenOrderSnapshot& lhs, const ExchangeOpenOrderSnapshot& rhs) {
                return lhs.local_order_id < rhs.local_order_id;
            }
        );
        return result;
    }

    void processPositionStopBar(Timestamp timestamp, const CoinBarMap& bars, const std::string& messageId)
    {
        if (timestamp <= last_position_stop_bar_) {
            // Retry a failed checkpoint/publication, but never rematch an already closed
            // bar against stops submitted later (including after a process restart).
            // A non-triggering bar also advances the cursor, even without events.
            checkpointTransition(messageId);
            return;
        }
        exchange_.processPositionStopsAtClose(timestamp, bars, account_.positions());
        captureExchangeEvents();
        last_position_stop_bar_ = timestamp;
        latest_timestamp_ = std::max(latest_timestamp_, timestamp);
        checkpointTransition(messageId);
    }

    DurableMessageDisposition onBars(const BusMessage& message)
    {
        try {
            const auto slice = MessageJson::decodeMarketSliceSnapshot(message.payload);
            validateMetadata(slice.metadata);
            if (slice.timestamp == 0)
                return DurableMessageDisposition::Terminate;
            if (!businessTimeReady(slice.metadata.produced_at, "position_stop_bar_metadata") ||
                !businessTimeReady(slice.timestamp, "position_stop_bar"))
                return DurableMessageDisposition::Retry;
            CoinBarMap bars;
            for (const auto& row : slice.bars) {
                BarData bar{row.bar.open, row.bar.high, row.bar.low, row.bar.close, row.bar.volume};
                bar.quote_volume = row.bar.quote_volume;
                if (row.coin.empty() || !bars.emplace(row.coin, bar).second)
                    throw std::invalid_argument("Invalid or duplicate coin in position stop bar");
            }
            processPositionStopBar(slice.timestamp, bars, slice.metadata.message_id);
            return DurableMessageDisposition::Ack;
        }
        catch (const std::invalid_argument& error) {
            LG_WARN("Rejected invalid position stop bars: {}", error.what());
            return DurableMessageDisposition::Terminate;
        }
        catch (const std::exception& error) {
            LG_ERROR("Position stop bar handling failed; will retry: {}", error.what());
            return DurableMessageDisposition::Retry;
        }
    }

    DurableMessageDisposition onMarketUpdate(const BusMessage& message)
    {
        try {
            const auto update = MessageJson::decodeMarketDataUpdated(message.payload);
            validateMetadata(update.metadata);
            if (update.completed_through == 0 || update.timeframe != "1d")
                return DurableMessageDisposition::Terminate;
            if (!businessTimeReady(update.metadata.produced_at, "position_stop_market_metadata") ||
                !businessTimeReady(update.completed_through, "position_stop_market_day"))
                return DurableMessageDisposition::Retry;
            const Timestamp through = update.completed_through;
            if (through <= last_position_stop_bar_) {
                processPositionStopBar(through, {}, update.metadata.message_id);
                return DurableMessageDisposition::Ack;
            }
            std::set<Coin> symbols;
            Timestamp first = through;
            for (const auto& [id, order] : exchange_.activeOrders()) {
                (void)id;
                if (order.isPositionStop() && order.active_from <= through) {
                    symbols.insert(order.coin);
                    first = std::min(first, order.active_from);
                }
            }
            if (!symbols.empty()) {
                if (last_position_stop_bar_ != 0)
                    first = std::max(first, static_cast<Timestamp>(nextDay(last_position_stop_bar_)));
                const auto days = (toUnixMillis(static_cast<int>(through)) -
                                   toUnixMillis(static_cast<int>(first))) / 86400000 + 1;
                CanonicalMarketDataReader reader(options_.market_data_db);
                const auto history = reader.loadMarketDataWindow(through, static_cast<unsigned int>(days), symbols);
                // Missing bars are unavailable evidence, not a synthetic zero low or a
                // reason to advance the durable cursor past an unobserved day.
                for (Timestamp day = first; day <= through; day = static_cast<Timestamp>(nextDay(day))) {
                    const auto found = history.find(day);
                    if (found == history.end())
                        throw std::runtime_error("Missing completed daily bars for position stops");
                    for (const auto& [id, order] : exchange_.activeOrders()) {
                        (void)id;
                        if (order.isPositionStop() && order.active_from <= day && !found->second.contains(order.coin))
                            throw std::runtime_error("Missing completed position stop coin bar");
                    }
                    processPositionStopBar(day, found->second, update.metadata.message_id);
                }
            }
            else {
                processPositionStopBar(through, {}, update.metadata.message_id);
            }
            return DurableMessageDisposition::Ack;
        }
        catch (const std::invalid_argument& error) {
            LG_WARN("Rejected invalid position stop market update: {}", error.what());
            return DurableMessageDisposition::Terminate;
        }
        catch (const std::exception& error) {
            LG_ERROR("Position stop OHLCV read failed; will retry: {}", error.what());
            return DurableMessageDisposition::Retry;
        }
    }

    DurableMessageDisposition onPrices(const BusMessage& message)
    {
        try {
            const ExecutionPriceSnapshot prices =
                MessageJson::decodeExecutionPriceSnapshot(message.payload);
            validateMetadata(prices.metadata);
            if (prices.timestamp == 0 || prices.prices.empty())
                return DurableMessageDisposition::Terminate;
            if (!businessTimeReady(prices.metadata.produced_at, "execution_prices_metadata") ||
                !businessTimeReady(prices.timestamp, "execution_prices_market_time")) {
                return DurableMessageDisposition::Retry;
            }

            LG_INFO("Received {} execution prices for {} (decision {})",
                    prices.prices.size(), FormatDayKey(prices.timestamp), FormatDayKey(prices.decision_timestamp));
            LG_DEBUG("Execution prices message id {}", prices.metadata.message_id);

            const auto existing = execution_prices_.find(prices.timestamp);
            if (existing != execution_prices_.end()) {
                if (!samePrices(existing->second, prices.prices))
                    return DurableMessageDisposition::Terminate;
                flushPendingEvents(prices.metadata.message_id);
                return DurableMessageDisposition::Ack;
            }

            // With canonical OHLCV configured, settle the preceding close before any
            // following open can mutate the holdings used by a reduce-only stop.
            if (!options_.market_data_db.empty() && prices.decision_timestamp > last_position_stop_bar_) {
                for (const auto& [id, order] : exchange_.activeOrders()) {
                    (void)id;
                    if (order.isPositionStop() && order.active_from <= prices.decision_timestamp)
                        return DurableMessageDisposition::Retry;
                }
            }

            if (latest_timestamp_ != 0 && prices.timestamp < latest_timestamp_)
                return DurableMessageDisposition::Terminate;

            execution_prices_.emplace(prices.timestamp, prices.prices);
            latest_timestamp_ = std::max(latest_timestamp_, prices.timestamp);

            // Fill any already-active orders at this exact simulated open.
            exchange_.processOpen(prices.timestamp, openBars(prices.prices));
            captureExchangeEvents();
            checkpointTransition(
                prices.metadata.message_id,
                std::make_optional(std::make_pair(prices.timestamp, prices.prices))
            );
            LG_INFO("Processed the open of {}: cash {}, {} positions, {} active orders",
                    FormatDayKey(prices.timestamp),
                    FormatUsd(account_.cash()),
                    account_.positions().values().size(),
                    exchange_.activeOrders().size());
            LG_DEBUG("Known orders after the open: {}", known_orders_.size());
            return DurableMessageDisposition::Ack;
        }
        catch (const std::invalid_argument& error) {
            LG_WARN("Rejected invalid execution prices: {}", error.what());
            return DurableMessageDisposition::Terminate;
        }
        catch (const std::exception& error) {
            LG_ERROR("Execution price handling failed; will retry: {}", error.what());
            return DurableMessageDisposition::Retry;
        }
    }

    DurableMessageDisposition onSubmit(const BusMessage& message)
    {
        try {
            const SubmitOrderCommand command =
                MessageJson::decodeSubmitOrderCommand(message.payload);
            validateMetadata(command.metadata);
            if (command.order.order_id == 0)
                return DurableMessageDisposition::Terminate;
            if (!businessTimeReady(command.metadata.produced_at, "backend_submit_order_metadata") ||
                !businessTimeReady(command.order.created_at, "backend_submit_order_created_at")) {
                return DurableMessageDisposition::Retry;
            }

            LG_INFO("Received order {}: {} {} {} for {}, active from {}",
                    command.order.order_id,
                    command.order.side == OrderSide::Buy ? "BUY" : "SELL",
                    FormatQuantity(command.order.quantity),
                    command.order.coin,
                    command.order.strategy_id,
                    FormatDayKey(command.order.active_from));
            LG_DEBUG("Order {} submit message id {}", command.order.order_id, command.metadata.message_id);

            const auto known = known_orders_.find(command.order.order_id);
            if (known != known_orders_.end()) {
                if (!sameOrder(known->second, command.order)) {
                    LG_ALERT("Order {} was submitted again with different content; rejecting it", command.order.order_id);
                    return DurableMessageDisposition::Terminate;
                }
                LG_INFO("Order {} was already received; ignoring the duplicate", command.order.order_id);
                flushPendingEvents(command.metadata.message_id);
                return DurableMessageDisposition::Ack;
            }

            known_orders_.emplace(command.order.order_id, command.order);
            exchange_.submitOrder(command.order);
            captureExchangeEvents();

            if (command.order.fill_at_reference_price) {
                // A manual order confirmed during the day: fill it now at the market price
                // it was planned with, not at a daily open.
                exchange_.fillAtPrice(command.order.order_id, command.order.active_from,
                                      command.order.reference_price);
                captureExchangeEvents();
                LG_INFO("Filled manual order {} at once at {}", command.order.order_id,
                        FormatQuantity(command.order.reference_price));
            }

            // In distributed replay the T+1 open commonly arrives before the resulting
            // SubmitOrder command. Use the order's exact active_from price, never a newer
            // "latest" price, so transport latency cannot change backtest semantics.
            processOpenIfAvailable(command.order);
            checkpointTransition(
                command.metadata.message_id,
                std::nullopt,
                std::make_optional(command.order)
            );
            LG_INFO("Accepted order {}: cash {}, {} positions, {} active orders",
                    command.order.order_id,
                    FormatUsd(account_.cash()),
                    account_.positions().values().size(),
                    exchange_.activeOrders().size());
            LG_DEBUG("Known orders: {}", known_orders_.size());
            return DurableMessageDisposition::Ack;
        }
        catch (const std::invalid_argument& error) {
            LG_WARN("Rejected invalid order: {}", error.what());
            return DurableMessageDisposition::Terminate;
        }
        catch (const std::exception& error) {
            LG_ERROR("Order handling failed; will retry: {}", error.what());
            return DurableMessageDisposition::Retry;
        }
    }

    DurableMessageDisposition onCancel(const BusMessage& message)
    {
        try {
            const CancelOrderCommand command =
                MessageJson::decodeCancelOrderCommand(message.payload);
            validateMetadata(command.metadata);
            if (command.order_id == 0)
                return DurableMessageDisposition::Terminate;
            if (!businessTimeReady(command.metadata.produced_at, "backend_cancel_order_metadata") ||
                !businessTimeReady(command.requested_at, "backend_cancel_order_requested_at")) {
                return DurableMessageDisposition::Retry;
            }

            LG_INFO("Received cancel for order {} ({})", command.order_id, FormatDayKey(command.requested_at));
            LG_DEBUG("Cancel message id {}", command.metadata.message_id);
            exchange_.cancelOrderAt(command.order_id, command.requested_at);
            captureExchangeEvents();
            checkpointTransition(command.metadata.message_id);
            return DurableMessageDisposition::Ack;
        }
        catch (const std::invalid_argument& error) {
            LG_WARN("Rejected invalid cancel: {}", error.what());
            return DurableMessageDisposition::Terminate;
        }
        catch (const std::exception& error) {
            LG_ERROR("Cancel handling failed; will retry: {}", error.what());
            return DurableMessageDisposition::Retry;
        }
    }

    DurableMessageDisposition onSnapshotRequest(const BusMessage& message)
    {
        try {
            const ExchangeSnapshotRequest request =
                MessageJson::decodeExchangeSnapshotRequest(message.payload);
            validateMetadata(request.metadata);
            if (!businessTimeReady(request.metadata.produced_at, "backend_snapshot_request_metadata"))
                return DurableMessageDisposition::Retry;

            flushPendingEvents(request.metadata.message_id);

            ExchangeSnapshotEvent output;
            output.snapshot = snapshot();
            output.metadata = eventMetadata(
                "sim-snapshot:" + request.metadata.message_id,
                request.metadata.message_id,
                output.snapshot.timestamp
            );
            bus_.publish(
                MessageSubjects::BACKEND_EXCHANGE_SNAPSHOT,
                MessageJson::encode(output),
                output.metadata.message_id
            );
            LG_INFO("Published exchange snapshot for {}: cash {}, {} positions, {} open orders",
                    FormatDayKey(output.snapshot.timestamp),
                    FormatUsd(output.snapshot.cash),
                    output.snapshot.positions.size(),
                    output.snapshot.open_orders.size());
            LG_DEBUG("Exchange snapshot message id {}, correlation id {}",
                     output.metadata.message_id, output.metadata.correlation_id);
            return DurableMessageDisposition::Ack;
        }
        catch (const std::invalid_argument& error) {
            LG_WARN("Rejected invalid snapshot request: {}", error.what());
            return DurableMessageDisposition::Terminate;
        }
        catch (const std::exception& error) {
            LG_ERROR("Snapshot request failed; will retry: {}", error.what());
            return DurableMessageDisposition::Retry;
        }
    }

public:
    explicit SimulatedExchangeServiceRuntime(Options options)
        : options_(std::move(options)),
          bus_(options_.nats_url),
          time_handler_(TimeHandlerFactory::createFromEnvironment()),
          exchange_(options_.commission_rate),
          account_(options_.initial_cash)
    {
        bus_.ensureStream(options_.runtime_stream, MessageSubjects::tradingRuntimeSubjects());
        bus_.ensureStream(options_.backend_stream, MessageSubjects::exchangeBackendSubjects());

        recoverDurableState();

        if (envFlag("ALGOTRADING_CHAOS_REJECT_ONCE")) {
            exchange_.configureRejectNextOrder("CHAOS_REJECT_ONCE");
            LG_WARN("Fault test enabled: the next order will be rejected once");
        }

        if (envFlag("ALGOTRADING_CHAOS_PARTIAL_FILL_ONCE")) {
            const double fraction =
                envDouble("ALGOTRADING_CHAOS_PARTIAL_FILL_FRACTION", 0.40);
            exchange_.configureSplitNextFill(fraction);
            LG_WARN("Fault test enabled: the next fill will be split once (first part {})", fraction);
        }

        prices_subscription_ = bus_.subscribe(
            consumer(
                options_.runtime_stream,
                "simulated-exchange-prices",
                MessageSubjects::EXECUTION_PRICES
            ),
            [this](const BusMessage& message) { return onPrices(message); }
        );
        bars_subscription_ = bus_.subscribe(
            consumer(options_.runtime_stream, "simulated-exchange-position-stop-bars",
                     MessageSubjects::MARKET_SLICE_SNAPSHOT),
            [this](const BusMessage& message) { return onBars(message); }
        );
        if (!options_.market_data_db.empty()) {
            market_update_subscription_ = bus_.subscribe(
                consumer(options_.runtime_stream, "simulated-exchange-position-stop-market",
                         MessageSubjects::MARKET_DATA_UPDATED),
                [this](const BusMessage& message) { return onMarketUpdate(message); }
            );
        }
        submit_subscription_ = bus_.subscribe(
            consumer(
                options_.backend_stream,
                "simulated-exchange-submit",
                MessageSubjects::BACKEND_SUBMIT_ORDER
            ),
            [this](const BusMessage& message) { return onSubmit(message); }
        );
        cancel_subscription_ = bus_.subscribe(
            consumer(
                options_.backend_stream,
                "simulated-exchange-cancel",
                MessageSubjects::BACKEND_CANCEL_ORDER
            ),
            [this](const BusMessage& message) { return onCancel(message); }
        );
        snapshot_request_subscription_ = bus_.subscribe(
            consumer(
                options_.backend_stream,
                "simulated-exchange-snapshot-request",
                MessageSubjects::BACKEND_EXCHANGE_SNAPSHOT_REQUEST
            ),
            [this](const BusMessage& message) { return onSnapshotRequest(message); }
        );
    }

    ~SimulatedExchangeServiceRuntime()
    {
        bus_.close(snapshot_request_subscription_);
        bus_.close(cancel_subscription_);
        bus_.close(submit_subscription_);
        bus_.close(prices_subscription_);
        bus_.close(bars_subscription_);
        if (market_update_subscription_ != 0)
            bus_.close(market_update_subscription_);
    }

    void run()
    {
        LG_INFO("Simulated exchange ready (initial cash {}, commission rate {}, persistence {}, today {})",
                FormatUsd(options_.initial_cash),
                options_.commission_rate,
                checkpoint_store_ ? "PostgreSQL" : "disabled",
                FormatDayKey(currentBusinessUtcDate(time_handler_)));
        LG_DEBUG("Runtime stream {}, backend stream {}, poll timeout {} ms",
                 options_.runtime_stream, options_.backend_stream, options_.poll_timeout_ms);
        LG_INFO("Position stop bars: {}", options_.market_data_db.empty()
                ? "market.slice.snapshot.v1 (execution opens alone do not carry lows)"
                : "market.slice.snapshot.v1 and read-only canonical OHLCV on market.data.updated.v1");
        std::cout.flush();

        while (running.load()) {
            // Closed bars first: reduce-only stops see holdings before the following open.
            bus_.poll(bars_subscription_, 32, options_.poll_timeout_ms);
            if (market_update_subscription_ != 0)
                bus_.poll(market_update_subscription_, 32, options_.poll_timeout_ms);
            // Prices first: orders submitted later for the same active_from timestamp are
            // still filled from the stored exact open by onSubmit().
            bus_.poll(prices_subscription_, 32, options_.poll_timeout_ms);
            bus_.poll(snapshot_request_subscription_, 8, options_.poll_timeout_ms);
            bus_.poll(cancel_subscription_, 32, options_.poll_timeout_ms);
            bus_.poll(submit_subscription_, 32, options_.poll_timeout_ms);
        }

        LG_INFO("Shutdown requested ({} events still pending)", pending_events_.size());
        flushPendingEvents("simulated-exchange-shutdown");
        bus_.flush();
        LG_INFO("Shutdown complete");
    }
};

} // namespace


// Process entrypoint.

int main(int argc, char** argv)
{
    ServiceLogging::setup("simulated-exchange");

    try {
        std::signal(SIGINT, stopHandler);
        std::signal(SIGTERM, stopHandler);
        SimulatedExchangeServiceRuntime runtime(parseOptions(argc, argv));
        runtime.run();
        return 0;
    }
    catch (const std::exception& error) {
        LG_ALERT("Simulated exchange stopped by a fatal error: {}", error.what());
        return 1;
    }
}
