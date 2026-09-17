// ============================================================================
// LEARNING VERSION — Strategy service orchestrator
// ============================================================================
//
// Mental model:
//   NATS MarketSliceSnapshot
//          ↓
//   this service orchestrator
//          ↓
//   RollingMarketState → StrategySignalEngine / PureRSI
//          ↓
//   StrategyIntentBatch → NATS
//          ↓
//   PostgreSQL checkpoint → ACK input
//
// ============================================================================

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <libpq-fe.h>
#include <nlohmann/json.hpp>

#include "contract_json_codec.h"
#include "indicator_ranker.h"
#include "liquidity_universe.h"
#include "nats_jetstream_message_bus.h"
#include "pureRSI.h"
#include "rolling_market_state.h"
#include "service_logging.h"
#include "service_clock.h"
#include "strategy_signal_engine.h"
#include "strategy_signal_instance.h"
#include "transport_subjects.h"

namespace {

using json = nlohmann::json;
std::atomic<bool> running{true};

// Called by the operating system when the process receives SIGINT/SIGTERM.
// It does not immediately destroy anything; it simply flips `running` to false.
// The main polling loop sees that flag and performs a clean shutdown.
void stopHandler(int) { running.store(false); }

struct Options {
    std::string nats_url = "nats://127.0.0.1:4222";
    std::string stream = "ALGOTRADING_RUNTIME";
    std::string strategy_config = "config/strategies/pure_rsi_signal.json";
    std::string postgres;
    RuntimeMode runtime_mode = RuntimeMode::Live;
    std::string simulation_id;
    int poll_timeout_ms = 250;
};

// Prints the command-line options supported by this executable.
// This is only user/operator help; it does not participate in trading logic.
void printUsage()
{
    std::cout
        << "Usage: algotrading_strategy_service [options]\n"
        << "  --nats-url URL\n"
        << "  --stream NAME\n"
        << "  --strategy-config PATH\n"
        << "  --postgres CONNECTION_STRING\n"
        << "  --runtime-mode live|testnet|replay\n"
        << "  --simulation-id ID   optional REPLAY identity guard\n"
        << "  --poll-timeout-ms N\n";
}

// Reads command-line arguments supplied when the Docker/process starts.
// It builds one `Options` object containing things such as:
// NATS address, PostgreSQL connection, strategy config, runtime mode and timeout.
// It also rejects unknown/invalid arguments early so the service does not start
// with an ambiguous configuration.
Options parseOptions(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        // Small helper used only inside parseOptions().
        // It means: "this flag must have another argument after it".
        // Example: `--nats-url nats://nats:4222`.
        // If the value is missing, startup fails with a clear error.
        auto requireValue = [&](const char* option) -> std::string {
            if (i + 1 >= argc)
                throw std::invalid_argument(std::string(option) + " requires a value");
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h") {
            printUsage();
            std::exit(0);
        } else if (arg == "--nats-url") {
            options.nats_url = requireValue("--nats-url");
        } else if (arg == "--stream") {
            options.stream = requireValue("--stream");
        } else if (arg == "--strategy-config") {
            options.strategy_config = requireValue("--strategy-config");
        } else if (arg == "--postgres") {
            options.postgres = requireValue("--postgres");
        } else if (arg == "--runtime-mode") {
            options.runtime_mode = parseRuntimeMode(requireValue("--runtime-mode"));
        } else if (arg == "--simulation-id") {
            options.simulation_id = requireValue("--simulation-id");
        } else if (arg == "--poll-timeout-ms") {
            options.poll_timeout_ms = std::stoi(requireValue("--poll-timeout-ms"));
        } else {
            throw std::invalid_argument("Unknown option: " + arg);
        }
    }

    if (options.nats_url.empty() || options.stream.empty() || options.strategy_config.empty())
        throw std::invalid_argument("Strategy-service required string options cannot be empty");
    if (options.poll_timeout_ms <= 0)
        throw std::invalid_argument("--poll-timeout-ms must be positive");
    return options;
}

// Loads an entire text file into a std::string.
// In this service it is used to read the strategy configuration so the exact
// config can be associated with PostgreSQL recovery state.
std::string readTextFile(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Cannot open file: " + path);

    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    );
}

// RAII wrapper around PostgreSQL's raw `PGresult*`.
// libpq requires every result to be released with PQclear(). This wrapper makes
// that automatic, reducing the chance of memory leaks when exceptions occur.
class PgResult {
private:
    PGresult* result_ = nullptr;

public:
    // Takes ownership of a raw PostgreSQL query result.
// From this point onward this object is responsible for calling PQclear().
    explicit PgResult(PGresult* result) : result_(result) {}
    // Destructor: automatically frees the PostgreSQL result when the wrapper
// leaves scope, including when an exception is thrown.
    ~PgResult()
    {
        if (result_)
            PQclear(result_);
    }

    // Copying is forbidden because two wrappers must never believe they both
// own the same PGresult*. That could cause a double-free.
    PgResult(const PgResult&) = delete;
    PgResult& operator=(const PgResult&) = delete;

    // Move constructor: transfers ownership of the PostgreSQL result from
// another PgResult without copying the underlying pointer.
    PgResult(PgResult&& other) noexcept
        : result_(std::exchange(other.result_, nullptr))
    {}

    // Move assignment: releases any result currently owned here, then takes
// ownership of the other object's result.
    PgResult& operator=(PgResult&& other) noexcept
    {
        if (this != &other) {
            if (result_)
                PQclear(result_);
            result_ = std::exchange(other.result_, nullptr);
        }
        return *this;
    }

    // Gives callers access to the underlying libpq result without transferring
// ownership. The PgResult wrapper still remains responsible for freeing it.
    PGresult* get() const { return result_; }
};

// PostgreSQL persistence layer used specifically by the strategy service.
// Its job is to store enough durable information to recover after a crash:
//   timestamp + input MarketSliceSnapshot + resulting StrategyIntentBatch.
// It also stores the exact strategy configuration associated with that history.
class StrategyCheckpointStore {
private:
    PGconn* connection_ = nullptr;
    static constexpr const char* STATE_KEY = "strategy-service";

    // Safety check used before every SQL operation.
// It refuses to execute a query unless the PostgreSQL connection is healthy.
    void requireConnection() const
    {
        if (connection_ == nullptr || PQstatus(connection_) != CONNECTION_OK)
            throw std::runtime_error("Strategy checkpoint PostgreSQL connection is not ready");
    }

    // Executes a normal SQL statement with no external parameters.
// It verifies PostgreSQL returned the expected result type and converts database
// failures into C++ exceptions. The returned PgResult cleans itself up.
    PgResult exec(const std::string& sql, ExecStatusType expected) const
    {
        requireConnection();
        PgResult result(PQexec(connection_, sql.c_str()));
        if (result.get() == nullptr || PQresultStatus(result.get()) != expected) {
            const std::string error = connection_ ? PQerrorMessage(connection_) : "unknown PostgreSQL error";
            throw std::runtime_error("Strategy checkpoint PostgreSQL query failed: " + error);
        }
        return result;
    }

    // Executes parameterized SQL (`$1`, `$2`, ...).
// Parameterized queries keep values separate from SQL text and are safer and
// easier to reason about than manually concatenating values into a query.
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
            throw std::runtime_error("Strategy checkpoint PostgreSQL parameterized query failed: " + error);
        }
        return result;
    }

    // Ensures the two strategy checkpoint tables exist.
// It also records the exact strategy configuration and checks that an existing
// checkpoint was produced with the SAME config. If the config changed, recovery
// is refused rather than silently mixing old state with new strategy rules.
    void ensureSchema(const std::string& strategyConfig)
    {
        exec(
            "CREATE TABLE IF NOT EXISTS strategy_service_metadata ("
            "state_key TEXT PRIMARY KEY, "
            "strategy_config TEXT NOT NULL"
            ")",
            PGRES_COMMAND_OK
        );

        exec(
            "CREATE TABLE IF NOT EXISTS strategy_market_slice_checkpoint ("
            "state_key TEXT NOT NULL, "
            "timestamp BIGINT NOT NULL, "
            "slice_payload TEXT NOT NULL, "
            "intent_payload TEXT NOT NULL, "
            "PRIMARY KEY(state_key, timestamp)"
            ")",
            PGRES_COMMAND_OK
        );

        execParams(
            "INSERT INTO strategy_service_metadata(state_key, strategy_config) "
            "VALUES($1, $2) ON CONFLICT(state_key) DO NOTHING",
            {STATE_KEY, strategyConfig},
            PGRES_COMMAND_OK
        );

        const PgResult metadata = execParams(
            "SELECT strategy_config FROM strategy_service_metadata WHERE state_key = $1",
            {STATE_KEY},
            PGRES_TUPLES_OK
        );

        if (PQntuples(metadata.get()) != 1)
            throw std::runtime_error("Strategy checkpoint metadata row is missing");

        const std::string persistedConfig = PQgetvalue(metadata.get(), 0, 0);
        if (persistedConfig != strategyConfig)
            throw std::runtime_error(
                "Persisted strategy checkpoint belongs to a different strategy configuration"
            );
    }

public:
    // Opens the PostgreSQL connection for strategy recovery/checkpointing.
// Startup fails if PostgreSQL cannot be reached. Once connected it also calls
// ensureSchema() to create/validate the durable storage structure.
    StrategyCheckpointStore(
        const std::string& connectionString,
        const std::string& strategyConfig
    )
    {
        connection_ = PQconnectdb(connectionString.c_str());
        if (connection_ == nullptr || PQstatus(connection_) != CONNECTION_OK) {
            const std::string error = connection_ ? PQerrorMessage(connection_) : "cannot allocate PGconn";
            if (connection_) {
                PQfinish(connection_);
                connection_ = nullptr;
            }
            throw std::runtime_error("Cannot connect strategy checkpoint store to PostgreSQL: " + error);
        }

        ensureSchema(strategyConfig);
    }

    // Closes the PostgreSQL connection when the checkpoint store is destroyed.
    ~StrategyCheckpointStore()
    {
        if (connection_)
            PQfinish(connection_);
    }

    // Copying is disabled because a live PostgreSQL connection has a single
// clear owner in this object.
    StrategyCheckpointStore(const StrategyCheckpointStore&) = delete;
    StrategyCheckpointStore& operator=(const StrategyCheckpointStore&) = delete;

    struct Row {
        Timestamp timestamp = 0;
        std::string slice_payload;
        std::string intent_payload;
    };

    // Loads ALL committed strategy checkpoints in chronological order.
// On restart, recoverFromCheckpoint() uses these rows to rebuild the in-memory
// RollingMarketState and restore the latest strategy signal state.
//
// NOTE FOR FUTURE DESIGN:
// This currently grows with history. We have already identified that production
// may later prefer "latest checkpoint + only the required warmup history" from
// the canonical market-data database instead of reloading unlimited history.
    std::vector<Row> loadAll() const
    {
        const PgResult result = execParams(
            "SELECT timestamp, slice_payload, intent_payload FROM strategy_market_slice_checkpoint "
            "WHERE state_key = $1 ORDER BY timestamp ASC",
            {STATE_KEY},
            PGRES_TUPLES_OK
        );

        std::vector<Row> rows;
        rows.reserve(static_cast<std::size_t>(PQntuples(result.get())));

        for (int row = 0; row < PQntuples(result.get()); ++row) {
            const std::string timestampText = PQgetvalue(result.get(), row, 0);
            const unsigned long long timestampValue = std::stoull(timestampText);
            if (timestampValue > static_cast<unsigned long long>(std::numeric_limits<Timestamp>::max()))
                throw std::runtime_error("Persisted strategy checkpoint timestamp is out of range");

            Row value;
            value.timestamp = static_cast<Timestamp>(timestampValue);
            value.slice_payload = PQgetvalue(result.get(), row, 1);
            value.intent_payload = PQgetvalue(result.get(), row, 2);
            rows.push_back(std::move(value));
        }

        return rows;
    }

    // Looks up the durable checkpoint for one exact timestamp.
// Returns no value if that timestamp has never been committed.
// This is used to recognize safe redeliveries/duplicates.
    std::optional<Row> rowFor(Timestamp timestamp) const
    {
        const PgResult result = execParams(
            "SELECT slice_payload, intent_payload FROM strategy_market_slice_checkpoint "
            "WHERE state_key = $1 AND timestamp = $2",
            {STATE_KEY, std::to_string(timestamp)},
            PGRES_TUPLES_OK
        );

        if (PQntuples(result.get()) == 0)
            return std::nullopt;
        if (PQntuples(result.get()) != 1)
            throw std::runtime_error("Strategy checkpoint timestamp is not unique");

        Row value;
        value.timestamp = timestamp;
        value.slice_payload = PQgetvalue(result.get(), 0, 0);
        value.intent_payload = PQgetvalue(result.get(), 0, 1);
        return value;
    }

    // Persists one completed strategy step:
//   input market slice + output strategy intent for the same timestamp.
//
// The insert deliberately does nothing on a duplicate primary key, then reads
// the existing row back and verifies it is IDENTICAL. Therefore receiving the
// same event again is safe, but conflicting history is treated as an error.
    void save(
        Timestamp timestamp,
        const std::string& slicePayload,
        const std::string& intentPayload
    )
    {
        execParams(
            "INSERT INTO strategy_market_slice_checkpoint("
            "state_key, timestamp, slice_payload, intent_payload"
            ") VALUES($1, $2, $3, $4) "
            "ON CONFLICT(state_key, timestamp) DO NOTHING",
            {STATE_KEY, std::to_string(timestamp), slicePayload, intentPayload},
            PGRES_COMMAND_OK
        );

        const std::optional<Row> persisted = rowFor(timestamp);
        if (!persisted.has_value() ||
            persisted->slice_payload != slicePayload ||
            persisted->intent_payload != intentPayload)
            throw std::logic_error("Conflicting persisted strategy checkpoint");
    }
};

// Converts an indicator name from JSON ("SMA", "RSI", "ATR", ...)
// into the C++ IndicatorKind enum understood by the indicator engine.
IndicatorKind parseIndicatorKind(const std::string& value)
{
    if (value == "SMA") return IndicatorKind::SMA;
    if (value == "EMA") return IndicatorKind::EMA;
    if (value == "RSI") return IndicatorKind::RSI;
    if (value == "ATR") return IndicatorKind::ATR;
    if (value == "ROC") return IndicatorKind::ROC;
    if (value == "Highest") return IndicatorKind::Highest;
    if (value == "Lowest") return IndicatorKind::Lowest;
    if (value == "DonchianHigh") return IndicatorKind::DonchianHigh;
    if (value == "DonchianLow") return IndicatorKind::DonchianLow;
    if (value == "DonchianMid") return IndicatorKind::DonchianMid;
    throw std::invalid_argument("Unsupported indicator kind: " + value);
}

// Converts the configured indicator input ("Open", "Close", "Volume", ...)
// into the C++ PriceField enum used by indicator calculations.
PriceField parsePriceField(const std::string& value)
{
    if (value == "Open") return PriceField::Open;
    if (value == "High") return PriceField::High;
    if (value == "Low") return PriceField::Low;
    if (value == "Close") return PriceField::Close;
    if (value == "Volume") return PriceField::Volume;
    throw std::invalid_argument("Unsupported indicator source: " + value);
}

// Builds one complete IndicatorSpec from JSON.
// Example: RSI of Close with length 7, or SMA of Volume with length 25.
// It also validates that the requested lookback length is not zero.
IndicatorSpec parseIndicatorSpec(const json& value)
{
    IndicatorSpec spec{
        parseIndicatorKind(value.at("kind").get<std::string>()),
        parsePriceField(value.at("source").get<std::string>()),
        value.at("length").get<unsigned int>(),
        value.value("offset", 0u)
    };
    if (spec.length == 0)
        throw std::invalid_argument("Indicator length must be positive");
    return spec;
}

// Builds the configured market-universe selector.
// For the current strategy this creates TopNLiquidityUniverse: rank coins by a
// configured liquidity indicator and keep only the top N eligible assets.
std::unique_ptr<UniverseSelector> makeUniverse(const json& value)
{
    const std::string type = value.at("type").get<std::string>();
    if (type != "top_n_liquidity")
        throw std::invalid_argument("Unsupported universe type: " + type);

    const unsigned int count = value.at("count").get<unsigned int>();
    if (count == 0)
        throw std::invalid_argument("Top-liquidity universe count must be positive");

    return std::make_unique<TopNLiquidityUniverse>(
        parseIndicatorSpec(value.at("indicator")),
        count,
        value.value("descending", true),
        true
    );
}

// Builds the ranking rule used after the universe is selected.
// The current configuration uses an indicator-based ranker (for PureRSI this is
// the RSI ranking), but this factory keeps the construction separate from the
// orchestration code.
std::unique_ptr<Ranker> makeRanker(const json& value)
{
    const std::string type = value.at("type").get<std::string>();
    if (type != "indicator")
        throw std::invalid_argument("Unsupported ranker type: " + type);

    return std::make_unique<IndicatorRanker>(
        parseIndicatorSpec(value.at("indicator")),
        value.value("descending", true),
        true
    );
}

// Reads the strategy JSON config and constructs the actual strategy objects.
// Today this service accepts strategies of type PureRSI. Parameters such as
// max signals, universe, ranking, RSI length, entry and exit thresholds come
// from the JSON file rather than being hardcoded in this orchestrator.
StrategySignalPortfolio loadStrategies(const std::string& path)
{
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("Cannot open strategy config: " + path);

    json root;
    input >> root;

    StrategySignalPortfolio strategies;
    std::unordered_set<StrategyID> ids;

    for (const json& value : root.at("strategies")) {
        const unsigned long parsedId = value.at("id").get<unsigned long>();
        if (parsedId > static_cast<unsigned long>(std::numeric_limits<StrategyID>::max()))
            throw std::invalid_argument("Strategy id is out of range");

        const StrategyID strategyId = static_cast<StrategyID>(parsedId);
        if (!ids.insert(strategyId).second)
            throw std::invalid_argument("Duplicate strategy id in strategy config");

        const std::string type = value.at("type").get<std::string>();
        if (type != "PureRSI")
            throw std::invalid_argument("Strategy type is not migrated to StrategySignalInstance: " + type);

        const json& parameters = value.at("parameters");
        strategies.emplace_back(
            strategyId,
            std::make_unique<StrategyPureRSI>(
                value.at("max_active_signals").get<unsigned int>(),
                makeUniverse(value.at("universe")),
                makeRanker(value.at("ranker")),
                value.at("max_ranking_position").get<unsigned int>(),
                parameters.at("rsi_length").get<unsigned int>(),
                parameters.at("rsi_entry").get<double>(),
                parameters.at("rsi_exit").get<double>()
            )
        );
    }

    if (strategies.empty())
        throw std::invalid_argument("Strategy config must contain at least one strategy");
    return strategies;
}

// Creates a deterministic message ID for the strategy output of one timestamp.
// If the same candle must be recomputed after a crash, the same timestamp creates
// the same ID, allowing JetStream to deduplicate the republished logical message.
std::string intentMessageId(Timestamp timestamp)
{
    return "strategy-intents:" + std::to_string(timestamp);
}

// The main ORCHESTRATOR for the strategy Docker/process.
// It owns the communication bus, clock context, PostgreSQL checkpoint store,
// in-memory rolling market history, strategy engine and NATS subscription.
class StrategyServiceRuntime {
private:
    const Options options_;
    NatsJetStreamMessageBus bus_;
    std::unique_ptr<ServiceClockContext> clock_;
    std::unique_ptr<StrategyCheckpointStore> checkpoint_store_;
    RollingMarketState market_state_;
    std::unique_ptr<StrategySignalEngine> engine_;
    DurableMessageBus::SubscriptionID market_subscription_ = 0;

    // Clears volatile strategy state in RAM and rebuilds a fresh strategy engine
// from the configured strategy JSON. This does NOT delete durable PostgreSQL data.
// Recovery calls this first, then reconstructs RAM from committed checkpoints.
    void resetRuntimeState()
    {
        market_state_ = RollingMarketState{};
        engine_ = std::make_unique<StrategySignalEngine>(loadStrategies(options_.strategy_config));
    }

    // Reconstructs the strategy service after a restart or when RAM can no longer
// be trusted. It loads committed rows from PostgreSQL in timestamp order,
// rebuilds RollingMarketState from the saved market slices, and restores the
// latest strategy intent so persistent signals continue correctly.
    void recoverFromCheckpoint()
    {
        if (!checkpoint_store_)
            return;

        resetRuntimeState();
        const auto rows = checkpoint_store_->loadAll();

        Timestamp latest = 0;
        std::optional<StrategyIntentBatch> latestIntent;
        for (const StrategyCheckpointStore::Row& row : rows) {
            const MarketSliceSnapshot slice = ContractJsonCodec::decodeMarketSliceSnapshot(row.slice_payload);
            if (slice.timestamp != row.timestamp)
                throw std::logic_error("Persisted strategy checkpoint timestamp/slice mismatch");
            if (!market_state_.append(slice))
                throw std::logic_error("Duplicate slice found inside persisted strategy checkpoint");

            StrategyIntentBatch intent = ContractJsonCodec::decodeStrategyIntentBatch(row.intent_payload);
            if (intent.timestamp != row.timestamp)
                throw std::logic_error("Persisted strategy checkpoint timestamp/intent mismatch");

            latest = row.timestamp;
            latestIntent = std::move(intent);
        }

        if (latestIntent.has_value())
            engine_->restore(*latestIntent);

        LG_INFO(
            "service=strategy event=strategy_recovery_completed recovered_slices={} latest_timestamp={}",
            rows.size(),
            latest
        );
    }

    // Core event handler: called whenever NATS delivers a MarketSliceSnapshot.
//
// High-level sequence:
//   1. Decode and validate the incoming market slice.
//   2. Check PostgreSQL for an already-committed copy of this timestamp.
//   3. Repair volatile RAM from PostgreSQL if a previous attempt died mid-step.
//   4. Append the new slice to RollingMarketState.
//   5. Run StrategySignalEngine / PureRSI for this close.
//   6. Publish StrategyIntentBatch to NATS.
//   7. Save input + output in PostgreSQL.
//   8. ACK the input.
//
// Return value tells JetStream what to do:
//   Ack       = successfully completed / known duplicate.
//   Retry     = temporary failure; please redeliver.
//   Terminate = permanently invalid/conflicting message; retrying will not help.
    DurableMessageDisposition onMarketSlice(const BusMessage& message)
    {
        try {
            const MarketSliceSnapshot slice = ContractJsonCodec::decodeMarketSliceSnapshot(message.payload);
            if (slice.timestamp == 0 || slice.bars.empty()) {
                LG_WARN(
                    "service=strategy event=market_slice_terminated reason=invalid_slice timestamp={} bars={} message_id={}",
                    slice.timestamp,
                    slice.bars.size(),
                    slice.metadata.message_id
                );
                return DurableMessageDisposition::Terminate;
            }

            LG_DEBUG(
                "service=strategy event=market_slice_received timestamp={} bars={} message_id={} correlation_id={}",
                slice.timestamp,
                slice.bars.size(),
                slice.metadata.message_id,
                slice.metadata.correlation_id
            );

            if (checkpoint_store_) {
                const std::optional<StrategyCheckpointStore::Row> persisted = checkpoint_store_->rowFor(slice.timestamp);
                if (persisted.has_value()) {
                    if (persisted->slice_payload != message.payload) {
                        LG_ALERT(
                            "service=strategy event=checkpoint_conflict timestamp={} disposition=terminate",
                            slice.timestamp
                        );
                        return DurableMessageDisposition::Terminate;
                    }

                    if (market_state_.empty() || market_state_.latestTimestamp() < slice.timestamp)
                        recoverFromCheckpoint();

                    LG_INFO(
                        "service=strategy event=market_slice_checkpoint_duplicate timestamp={} disposition=ack",
                        slice.timestamp
                    );
                    return DurableMessageDisposition::Ack;
                }

                // A previous attempt may have advanced RAM but failed before the durable
                // checkpoint. Rebuild only from committed slices before retrying so the
                // strategy engine again processes this timestamp exactly once locally.
                if (!market_state_.empty() && slice.timestamp <= market_state_.latestTimestamp()) {
                    LG_WARN(
                        "service=strategy event=volatile_state_ahead_of_checkpoint timestamp={} latest_runtime_timestamp={} action=recover",
                        slice.timestamp,
                        market_state_.latestTimestamp()
                    );
                    recoverFromCheckpoint();
                }
            }

            if (!market_state_.append(slice)) {
                LG_INFO(
                    "service=strategy event=market_slice_duplicate timestamp={} disposition=ack",
                    slice.timestamp
                );
                return DurableMessageDisposition::Ack;
            }

            StrategyIntentBatch output = engine_->onBarClose(
                market_state_.rawData(),
                market_state_.marketData(),
                slice.timestamp
            );

            output.metadata.schema_version = 1;
            output.metadata.message_id = intentMessageId(slice.timestamp);
            output.metadata.correlation_id = !slice.metadata.correlation_id.empty()
                ? slice.metadata.correlation_id
                : slice.metadata.message_id;
            output.metadata.produced_at = slice.timestamp;

            // Publish first, then checkpoint, then ACK. Because the output MessageID is
            // deterministic, a crash after publish but before checkpoint safely republishes
            // the same logical message after recovery and JetStream deduplicates it.
            const std::string encodedIntent = ContractJsonCodec::encode(output);
            bus_.publish(
                TransportSubjects::STRATEGY_INTENTS,
                encodedIntent,
                output.metadata.message_id
            );

            std::size_t activeSignals = 0;
            for (const StrategySignalIntent& strategy : output.strategies)
                activeSignals += strategy.signals.size();

            LG_INFO(
                "service=strategy event=strategy_intents_published timestamp={} strategies={} active_signals={} message_id={} correlation_id={}",
                output.timestamp,
                output.strategies.size(),
                activeSignals,
                output.metadata.message_id,
                output.metadata.correlation_id
            );

            if (checkpoint_store_) {
                checkpoint_store_->save(slice.timestamp, message.payload, encodedIntent);
                LG_DEBUG(
                    "service=strategy event=strategy_checkpoint_committed timestamp={} message_id={}",
                    slice.timestamp,
                    slice.metadata.message_id
                );
            }

            return DurableMessageDisposition::Ack;
        }
        catch (const std::logic_error& error) {
            LG_WARN("service=strategy event=market_slice_rejected disposition=terminate error={}", error.what());
            return DurableMessageDisposition::Terminate;
        }
        catch (const std::exception& error) {
            LG_ERROR("service=strategy event=market_slice_failed disposition=retry error={}", error.what());
            return DurableMessageDisposition::Retry;
        }
    }

public:
    // Starts and wires the whole strategy service.
//
// It:
//   - connects to NATS/JetStream,
//   - sets up LIVE/TESTNET/REPLAY clock behaviour,
//   - creates the PureRSI strategy engine,
//   - connects to PostgreSQL and performs crash recovery when configured,
//   - creates the durable MarketSlice consumer,
//   - registers onMarketSlice() as the callback.
//
// After this constructor succeeds, the service is ready for run().
    explicit StrategyServiceRuntime(Options options)
        : options_(std::move(options)),
          bus_(options_.nats_url)
    {
        // Bind the REPLAY clock control plane before checkpoint reconstruction.  Database
        // recovery is business-state work and must not postpone restart re-synchronization.
        bus_.ensureStream(
            options_.stream,
            options_.runtime_mode == RuntimeMode::Replay
                ? TransportSubjects::runtimeSubjects()
                : TransportSubjects::tradingRuntimeSubjects()
        );
        clock_ = std::make_unique<ServiceClockContext>(
            ServiceClockContext::Options{
                options_.runtime_mode, options_.stream, "strategy", options_.simulation_id
            },
            bus_
        );

        resetRuntimeState();

        if (!options_.postgres.empty()) {
            checkpoint_store_ = std::make_unique<StrategyCheckpointStore>(
                options_.postgres,
                readTextFile(options_.strategy_config)
            );
            recoverFromCheckpoint();
        } else {
            LG_WARN(
                "service=strategy event=restart_checkpoint_disabled reason=postgres_not_configured"
            );
        }

        DurableConsumerOptions consumer;
        consumer.stream = options_.stream;
        consumer.durable_name = "strategy-service-market-slices";
        consumer.subject = TransportSubjects::MARKET_SLICE_SNAPSHOT;
        consumer.ack_wait_ms = 30000;
        consumer.max_deliver = 20;
        consumer.max_ack_pending = 64;

        market_subscription_ = bus_.subscribe(
            consumer,
            clock_->guard(
                "market_slice",
                // Callback given to NATS: whenever a market-slice message arrives,
                // forward it to this StrategyServiceRuntime's onMarketSlice().
                [this](const BusMessage& message) { return onMarketSlice(message); }
            )
        );
    }

    // Destructor: closes the durable NATS subscription owned by this service.
    ~StrategyServiceRuntime() { bus_.close(market_subscription_); }

    // Main event loop of the Docker/process.
// Most of the time the strategy service simply waits for events.
// Each iteration lets the clock context process any clock/control messages and
// lets NATS deliver available MarketSlice messages to onMarketSlice().
// When stopHandler() flips `running` to false, it flushes NATS and exits cleanly.
    void run()
    {
        LG_INFO(
            "service=strategy event=service_ready stream={} config={} restart_checkpoint={} runtime_mode={} clock_sync={} poll_timeout_ms={}",
            options_.stream,
            options_.strategy_config,
            checkpoint_store_ ? "postgres" : "disabled",
            runtimeModeName(options_.runtime_mode),
            clock_->synchronized() ? "ready" : "pending",
            options_.poll_timeout_ms
        );
        std::cout.flush();

        while (running.load()) {
            clock_->poll(32, 1);
            bus_.poll(market_subscription_, 16, options_.poll_timeout_ms);
        }

        LG_INFO("service=strategy event=shutdown_requested");
        bus_.flush();
        LG_INFO("service=strategy event=shutdown_complete");
    }
};

} // namespace

// Program entry point.
// It sets up logging and clean-shutdown signal handlers, parses startup options,
// constructs the StrategyServiceRuntime orchestrator and then runs it.
// Any unrecoverable startup/runtime exception is logged as fatal and returns 1.
int main(int argc, char** argv)
{
    ServiceLogging::setup("strategy");

    try {
        std::signal(SIGINT, stopHandler);
        std::signal(SIGTERM, stopHandler);
        StrategyServiceRuntime runtime(parseOptions(argc, argv));
        runtime.run();
        return 0;
    }
    catch (const std::exception& error) {
        LG_ALERT("service=strategy event=fatal error={}", error.what());
        return 1;
    }
}
