// Reads committed market data, evaluates configured strategies, and publishes strategy intents.
//
// 1. Load strategy configuration and recover the latest durable checkpoint.
// 2. Consume MarketDataUpdated only for completed business dates.
// 3. Warm/read canonical history, calculate intents, persist the checkpoint, and publish
// StrategyIntentBatch.

#include <algorithm>
#include <atomic>
#include <csignal>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include <libpq-fe.h>
#include <nlohmann/json.hpp>

#include "canonical_market_data_reader.h"
#include "message_json.h"
#include "indicator_ranker.h"
#include "liquidity_universe.h"
#include "market_messages.h"
#include "jetstream_bus.h"
#include "validated/pure_rsi.h"
#include "service_logging.h"
#include "strategy_signal_engine.h"
#include "strategy_signal_instance.h"
#include "time_handler_factory.h"
#include "time_utils.h"
#include "message_subjects.h"


// Internal helpers and service implementation.

namespace {

using json = nlohmann::json;
std::atomic<bool> running{true};

void stopHandler(int) { running.store(false); }


// Command-line configuration.

struct Options {
    std::string nats_url = "nats://127.0.0.1:4222";
    std::string stream = "ALGOTRADING_RUNTIME";
    std::string strategy_config = "config/strategies/pure_rsi_signal.json";
    std::filesystem::path market_data_db = "storage/databases/database.db";
    unsigned int market_warmup_days = 100;
    unsigned int market_top_n = 50;
    std::string market_data_source = "binance";
    std::string postgres;
};

void printUsage()
{
    std::cout
        << "Usage: algotrading_strategy_service [options]\n"
        << "  --nats-url URL\n"
        << "  --stream NAME\n"
        << "  --strategy-config PATH\n"
        << "  --market-data-db PATH       canonical SQLite market database\n"
        << "  --market-warmup-days N      default 100\n"
        << "  --market-top-n N            canonical entry candidate cap, default 50\n"
        << "  --market-data-source NAME   expected MarketDataUpdated source, default binance\n"
        << "  --postgres CONNECTION_STRING\n"
        << "  Business time is provided by TimeHandler (identity UTC by default)\n";
}

Options parseOptions(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
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
        } else if (arg == "--market-data-db") {
            options.market_data_db = requireValue("--market-data-db");
        } else if (arg == "--market-warmup-days") {
            options.market_warmup_days = static_cast<unsigned int>(
                std::stoul(requireValue("--market-warmup-days")));
        } else if (arg == "--market-top-n") {
            options.market_top_n = static_cast<unsigned int>(
                std::stoul(requireValue("--market-top-n")));
        } else if (arg == "--market-data-source") {
            options.market_data_source = requireValue("--market-data-source");
        } else if (arg == "--postgres") {
            options.postgres = requireValue("--postgres");
        } else {
            throw std::invalid_argument("Unknown option: " + arg);
        }
    }

    if (options.nats_url.empty() || options.stream.empty() || options.strategy_config.empty())
        throw std::invalid_argument("Strategy-service required string options cannot be empty");
    if (options.market_data_db.empty())
        throw std::invalid_argument("--market-data-db cannot be empty");
    if (!std::filesystem::exists(options.market_data_db))
        throw std::invalid_argument("Canonical market-data SQLite database does not exist");
    if (options.market_warmup_days == 0)
        throw std::invalid_argument("--market-warmup-days must be positive");
    if (options.market_top_n == 0)
        throw std::invalid_argument("--market-top-n must be positive");
    if (options.market_data_source.empty())
        throw std::invalid_argument("--market-data-source cannot be empty");
    return options;
}

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

std::string checkpointIdentity(const Options& options)
{
    return readTextFile(options.strategy_config) +
        "\nmarket_data_mode=canonical-sqlite-v1" +
        "\nmarket_warmup_days=" + std::to_string(options.market_warmup_days) +
        "\nmarket_top_n=" + std::to_string(options.market_top_n) +
        "\nmarket_data_source=" + options.market_data_source;
}

Timestamp newestCompletedBusinessUtcDate(const TimeHandler& timeHandler)
{
    const auto todayUtc = getCurrentUtcDate(timeHandler.getTime());
    return static_cast<Timestamp>(toYYYYMMDD(getPreviousDayDate(todayUtc)));
}

std::optional<Timestamp> configuredBootstrapCompletedUtcDate(const TimeHandlerConfig& config)
{
    // An explicit shared simulated reference defines the immutable economic start of
    // accelerated/historical runs.  Derive the first completed UTC day from that
    // reference itself, not from getTime() sampled after process/container startup.
    //
    // Identity LIVE mode intentionally leaves both reference env vars absent; in that
    // case bootstrap remains anchored to the current canonical frontier as before.
    const char* simulatedReference = std::getenv(TimeHandlerFactory::SIMULATED_REFERENCE_ENV);
    if (!simulatedReference || *simulatedReference == '\0')
        return std::nullopt;

    const auto referenceUtcDate = getCurrentUtcDate(config.simulated_reference_utc);
    return static_cast<Timestamp>(toYYYYMMDD(getPreviousDayDate(referenceUtcDate)));
}

// BUSINESS wait: the deadline is evaluated against TimeHandler business time.
// The short real sleep is only a TECHNICAL polling cadence so shutdown remains responsive.
void interruptibleBusinessWaitUntil(
    const TimeHandler& timeHandler,
    std::chrono::system_clock::time_point businessDeadline)
{
    constexpr auto technicalPollInterval = std::chrono::milliseconds(250);

    while (running.load()) {
        if (timeHandler.getTime() >= businessDeadline)
            return;
        std::this_thread::sleep_for(technicalPollInterval);
    }
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

class StrategyCheckpointStore {
private:
    PGconn* connection_ = nullptr;
    static constexpr const char* STATE_KEY = "strategy-service-market-db-v1";

    void requireConnection() const
    {
        if (connection_ == nullptr || PQstatus(connection_) != CONNECTION_OK)
            throw std::runtime_error("Strategy checkpoint PostgreSQL connection is not ready");
    }

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
            connection_, sql.c_str(), static_cast<int>(values.size()), nullptr,
            values.data(), nullptr, nullptr, 0
        ));

        if (result.get() == nullptr || PQresultStatus(result.get()) != expected) {
            const std::string error = connection_ ? PQerrorMessage(connection_) : "unknown PostgreSQL error";
            throw std::runtime_error("Strategy checkpoint PostgreSQL parameterized query failed: " + error);
        }
        return result;
    }

    void ensureSchema(const std::string& identity)
    {
        exec(
            "CREATE TABLE IF NOT EXISTS strategy_service_metadata ("
            "state_key TEXT PRIMARY KEY, "
            "strategy_config TEXT NOT NULL"
            ")",
            PGRES_COMMAND_OK
        );

        // New Step-3 table.  The old strategy_market_slice_checkpoint table is left
        // untouched so previous REPLAY validation data is not silently reinterpreted.
        exec(
            "CREATE TABLE IF NOT EXISTS strategy_market_update_checkpoint ("
            "state_key TEXT NOT NULL, "
            "timestamp BIGINT NOT NULL, "
            "update_payload TEXT NOT NULL, "
            "intent_payload TEXT NOT NULL, "
            "PRIMARY KEY(state_key, timestamp)"
            ")",
            PGRES_COMMAND_OK
        );

        execParams(
            "INSERT INTO strategy_service_metadata(state_key, strategy_config) "
            "VALUES($1, $2) ON CONFLICT(state_key) DO NOTHING",
            {STATE_KEY, identity},
            PGRES_COMMAND_OK
        );

        const PgResult metadata = execParams(
            "SELECT strategy_config FROM strategy_service_metadata WHERE state_key = $1",
            {STATE_KEY},
            PGRES_TUPLES_OK
        );

        if (PQntuples(metadata.get()) != 1)
            throw std::runtime_error("Strategy checkpoint metadata row is missing");

        const std::string persistedIdentity = PQgetvalue(metadata.get(), 0, 0);
        if (persistedIdentity != identity)
            throw std::runtime_error(
                "Persisted strategy checkpoint belongs to a different strategy/market-window configuration");
    }

    static Timestamp parseTimestamp(const char* text)
    {
        const unsigned long long value = std::stoull(text ? text : "0");
        if (value > static_cast<unsigned long long>(std::numeric_limits<Timestamp>::max()))
            throw std::runtime_error("Persisted strategy checkpoint timestamp is out of range");
        return static_cast<Timestamp>(value);
    }

public:
    StrategyCheckpointStore(
        const std::string& connectionString,
        const std::string& identity
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

        ensureSchema(identity);
    }

    ~StrategyCheckpointStore()
    {
        if (connection_)
            PQfinish(connection_);
    }

    StrategyCheckpointStore(const StrategyCheckpointStore&) = delete;
    StrategyCheckpointStore& operator=(const StrategyCheckpointStore&) = delete;

    struct Row {
        Timestamp timestamp = 0;
        std::string update_payload;
        std::string intent_payload;
    };

    std::optional<Row> latest() const
    {
        const PgResult result = execParams(
            "SELECT timestamp, update_payload, intent_payload "
            "FROM strategy_market_update_checkpoint "
            "WHERE state_key = $1 ORDER BY timestamp DESC LIMIT 1",
            {STATE_KEY},
            PGRES_TUPLES_OK
        );

        if (PQntuples(result.get()) == 0)
            return std::nullopt;
        if (PQntuples(result.get()) != 1)
            throw std::runtime_error("Latest strategy checkpoint query returned multiple rows");

        Row value;
        value.timestamp = parseTimestamp(PQgetvalue(result.get(), 0, 0));
        value.update_payload = PQgetvalue(result.get(), 0, 1);
        value.intent_payload = PQgetvalue(result.get(), 0, 2);
        return value;
    }

    std::optional<Row> rowFor(Timestamp timestamp) const
    {
        const PgResult result = execParams(
            "SELECT update_payload, intent_payload FROM strategy_market_update_checkpoint "
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
        value.update_payload = PQgetvalue(result.get(), 0, 0);
        value.intent_payload = PQgetvalue(result.get(), 0, 1);
        return value;
    }

    void save(
        Timestamp timestamp,
        const std::string& updatePayload,
        const std::string& intentPayload
    )
    {
        execParams(
            "INSERT INTO strategy_market_update_checkpoint("
            "state_key, timestamp, update_payload, intent_payload"
            ") VALUES($1, $2, $3, $4) "
            "ON CONFLICT(state_key, timestamp) DO NOTHING",
            {STATE_KEY, std::to_string(timestamp), updatePayload, intentPayload},
            PGRES_COMMAND_OK
        );

        const std::optional<Row> persisted = rowFor(timestamp);
        if (!persisted.has_value() || persisted->intent_payload != intentPayload)
            throw std::logic_error("Conflicting persisted strategy checkpoint");
    }
};


// Strategy configuration parsing and construction.

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

PriceField parsePriceField(const std::string& value)
{
    if (value == "Open") return PriceField::Open;
    if (value == "High") return PriceField::High;
    if (value == "Low") return PriceField::Low;
    if (value == "Close") return PriceField::Close;
    if (value == "Volume") return PriceField::Volume;
    throw std::invalid_argument("Unsupported indicator source: " + value);
}

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

std::string intentMessageId(Timestamp timestamp)
{
    return "strategy-intents:" + std::to_string(timestamp);
}


// Service runtime and message-processing loop.

class StrategyServiceRuntime {
private:
    const Options options_;
    const TimeHandlerConfig time_config_;
    const TimeHandler time_handler_;
    const std::optional<Timestamp> bootstrap_completed_date_;
    JetStreamBus bus_;
    CanonicalMarketDataReader market_reader_;
    Timestamp durable_checkpoint_timestamp_ = 0;
    std::unique_ptr<StrategyCheckpointStore> checkpoint_store_;
    std::unique_ptr<StrategySignalEngine> engine_;
    MessageBus::SubscriptionID market_update_subscription_ = 0;

    void resetRuntimeState()
    {
        engine_ = std::make_unique<StrategySignalEngine>(loadStrategies(options_.strategy_config));
    }

    void recoverFromCheckpoint()
    {
        resetRuntimeState();
        durable_checkpoint_timestamp_ = 0;
        if (!checkpoint_store_)
            return;

        const std::optional<StrategyCheckpointStore::Row> row = checkpoint_store_->latest();
        if (!row.has_value()) {
            LG_INFO("service=strategy event=strategy_recovery_completed checkpoint=empty");
            return;
        }

        StrategyIntentBatch intent = MessageJson::decodeStrategyIntentBatch(row->intent_payload);
        if (intent.timestamp != row->timestamp)
            throw std::logic_error("Persisted strategy checkpoint timestamp/intent mismatch");
        engine_->restore(intent);
        durable_checkpoint_timestamp_ = row->timestamp;

        LG_INFO(
            "service=strategy event=strategy_recovery_completed checkpoint=latest-only latest_timestamp={}",
            row->timestamp
        );
    }

    std::set<Coin> activeSignalCoins() const
    {
        std::set<Coin> result;
        for (const StrategySignalInstance& strategy : engine_->strategies()) {
            for (const auto& [coin, signal] : strategy.signalState().values()) {
                if (signal != 0.0)
                    result.insert(coin);
            }
        }
        return result;
    }

    static bool sameLogicalUpdate(
        const MarketDataUpdated& left,
        const MarketDataUpdated& right)
    {
        return left.completed_through == right.completed_through &&
               left.source == right.source &&
               left.timeframe == right.timeframe &&
               left.active_top_n == right.active_top_n;
    }

    StrategyIntentBatch calculateForDate(Timestamp date)
    {
        const std::set<Coin> activeBefore = activeSignalCoins();
        const CanonicalMarketDataWindow window = market_reader_.loadWindow(
            date,
            options_.market_warmup_days,
            options_.market_top_n,
            activeBefore
        );

        LG_INFO(
            "service=strategy event=market_window_loaded timestamp={} warmup_days={} ranked_symbols={} active_history_symbols={} history_symbols={} history_rows={}",
            date,
            options_.market_warmup_days,
            window.ranked_symbols,
            activeBefore.size(),
            window.history_symbols,
            window.history_rows
        );

        return engine_->onBarClose(window.raw_data, window.market_data, date);
    }

    DurableMessageDisposition onMarketDataUpdated(const BusMessage& message)
    {
        try {
            const MarketDataUpdated update = MessageJson::decodeMarketDataUpdated(message.payload);

            if (update.source != options_.market_data_source || update.timeframe != "1d") {
                LG_WARN(
                    "service=strategy event=market_update_terminated reason=unsupported_source_or_timeframe source={} timeframe={}",
                    update.source,
                    update.timeframe
                );
                return DurableMessageDisposition::Terminate;
            }
            if (update.active_top_n != options_.market_top_n) {
                LG_ALERT(
                    "service=strategy event=market_update_terminated reason=top_n_config_mismatch event_top_n={} strategy_top_n={}",
                    update.active_top_n,
                    options_.market_top_n
                );
                return DurableMessageDisposition::Terminate;
            }

            const Timestamp target = update.completed_through;
            const Timestamp newestCompleted = newestCompletedBusinessUtcDate(time_handler_);
            if (target > newestCompleted) {
                LG_ALERT(
                    "service=strategy event=market_update_terminated reason=future_completed_date timestamp={} newest_completed_utc={} disposition=terminate",
                    target,
                    newestCompleted
                );
                return DurableMessageDisposition::Terminate;
            }

            // Restart/catch-up is anchored to market data that is actually canonical,
            // not merely to simulated time. During a Strategy outage TimeHandler may
            // advance beyond the newest SQLite commit. If stale detection used
            // newestCompleted directly, retained JetStream notifications could all be
            // ACKed away before the durable signal state has a chance to catch up.
            const std::optional<Timestamp> canonicalFrontierOpt =
                market_reader_.latestRankingDateAtOrBefore(newestCompleted);
            if (!canonicalFrontierOpt.has_value()) {
                LG_WARN(
                    "service=strategy event=market_update_failed reason=no_canonical_frontier timestamp={} newest_completed_utc={} disposition=retry",
                    target,
                    newestCompleted
                );
                return DurableMessageDisposition::Retry;
            }
            const Timestamp canonicalFrontier = *canonicalFrontierOpt;

            // Fresh historical/accelerated bootstrap must be invariant to wall-clock
            // startup latency and replay speed.  The immutable shared simulated reference
            // defines which UTC day was already complete at T_ref.  Retained notifications
            // before that day are warm-up backlog; the anchor day itself must be processed
            // even if the moving canonical frontier has advanced while the process starts.
            const bool freshBootstrap =
                engine_->lastTimestamp() == 0 && durable_checkpoint_timestamp_ == 0;
            const bool anchoredBootstrapTarget =
                freshBootstrap && bootstrap_completed_date_.has_value() &&
                target == *bootstrap_completed_date_;

            if (freshBootstrap && bootstrap_completed_date_.has_value()) {
                if (target < *bootstrap_completed_date_) {
                    LG_INFO(
                        "service=strategy event=pre_bootstrap_market_update_skipped timestamp={} bootstrap_completed_date={} canonical_frontier={} disposition=ack",
                        target,
                        *bootstrap_completed_date_,
                        canonicalFrontier
                    );
                    return DurableMessageDisposition::Ack;
                }

                if (target > *bootstrap_completed_date_) {
                    LG_WARN(
                        "service=strategy event=market_update_failed reason=awaiting_immutable_bootstrap_date timestamp={} bootstrap_completed_date={} canonical_frontier={} disposition=retry",
                        target,
                        *bootstrap_completed_date_,
                        canonicalFrontier
                    );
                    return DurableMessageDisposition::Retry;
                }
            }

            // Historical feeder is commit-before-publish. A notification newer than the
            // canonical SQLite frontier is therefore transient/inconsistent and must be
            // retried instead of ACKed. The frontier is capped by newestCompleted above,
            // so this cannot introduce lookahead.
            if (target > canonicalFrontier) {
                LG_WARN(
                    "service=strategy event=market_update_failed reason=canonical_frontier_not_visible timestamp={} canonical_frontier={} newest_completed_utc={} disposition=retry",
                    target,
                    canonicalFrontier,
                    newestCompleted
                );
                return DurableMessageDisposition::Retry;
            }

            // Drain retained notifications older than the latest canonical day without
            // emitting late economic output. The notification for canonicalFrontier then
            // runs calculateForDate() for every missing date since the durable checkpoint
            // and publishes only the frontier day's intent.
            if (target < canonicalFrontier && !anchoredBootstrapTarget) {
                LG_INFO(
                    "service=strategy event=stale_market_update_skipped timestamp={} canonical_frontier={} newest_completed_utc={} disposition=ack",
                    target,
                    canonicalFrontier,
                    newestCompleted
                );
                return DurableMessageDisposition::Ack;
            }

            LG_INFO(
                "service=strategy event=market_data_updated_received timestamp={} message_id={} database={}",
                target,
                update.metadata.message_id,
                options_.market_data_db.string()
            );

            if (checkpoint_store_) {
                const std::optional<StrategyCheckpointStore::Row> persisted = checkpoint_store_->rowFor(target);
                if (persisted.has_value()) {
                    const MarketDataUpdated persistedUpdate =
                        MessageJson::decodeMarketDataUpdated(persisted->update_payload);
                    if (!sameLogicalUpdate(persistedUpdate, update)) {
                        LG_ALERT(
                            "service=strategy event=market_update_checkpoint_conflict timestamp={} disposition=terminate",
                            target
                        );
                        return DurableMessageDisposition::Terminate;
                    }

                    if (engine_->lastTimestamp() < target)
                        recoverFromCheckpoint();
                    durable_checkpoint_timestamp_ = std::max(durable_checkpoint_timestamp_, target);

                    LG_INFO(
                        "service=strategy event=market_update_checkpoint_duplicate timestamp={} disposition=ack",
                        target
                    );
                    return DurableMessageDisposition::Ack;
                }

                // A failed previous attempt may have advanced volatile signal state before
                // PostgreSQL was committed.  Restore only the latest durable intent before
                // reprocessing the market update.
                if (engine_->lastTimestamp() >= target) {
                    LG_WARN(
                        "service=strategy event=volatile_state_ahead_of_checkpoint timestamp={} engine_timestamp={} action=recover",
                        target,
                        engine_->lastTimestamp()
                    );
                    recoverFromCheckpoint();
                }
            }

            if (engine_->lastTimestamp() > target) {
                LG_WARN(
                    "service=strategy event=stale_market_update timestamp={} engine_timestamp={} disposition=ack",
                    target,
                    engine_->lastTimestamp()
                );
                return DurableMessageDisposition::Ack;
            }

            std::vector<Timestamp> datesToProcess;
            if (engine_->lastTimestamp() == 0) {
                // First LIVE bootstrap: use bounded history to calculate indicators for the
                // newest completed day, but intentionally start persistent signal state empty.
                datesToProcess.push_back(target);
                LG_WARN(
                    "service=strategy event=live_signal_bootstrap timestamp={} policy=fresh_signal_state warmup_days={}",
                    target,
                    options_.market_warmup_days
                );
            } else {
                Timestamp date = nextDay(engine_->lastTimestamp());
                std::size_t guard = 0;
                while (date <= target) {
                    datesToProcess.push_back(date);
                    if (date == target)
                        break;
                    date = nextDay(date);
                    if (++guard > 3660)
                        throw std::logic_error("Strategy catch-up gap is unexpectedly large");
                }
            }

            if (datesToProcess.empty()) {
                LG_INFO(
                    "service=strategy event=market_update_duplicate_in_memory timestamp={} disposition=ack",
                    target
                );
                return DurableMessageDisposition::Ack;
            }

            StrategyIntentBatch output;
            for (const Timestamp date : datesToProcess) {
                if (!market_reader_.hasRankingDate(date))
                    throw std::logic_error(
                        "Canonical market database is missing market ranking for required date " +
                        std::to_string(date));

                output = calculateForDate(date);
                if (date != target) {
                    LG_INFO(
                        "service=strategy event=signal_state_catchup_processed timestamp={} published=false",
                        date
                    );
                }
            }

            output.metadata.schema_version = 1;
            output.metadata.message_id = intentMessageId(target);
            output.metadata.correlation_id = !update.metadata.correlation_id.empty()
                ? update.metadata.correlation_id
                : update.metadata.message_id;
            output.metadata.produced_at = target;

            // Same crash contract as before: deterministic publish first, durable checkpoint
            // second, ACK third. A crash after publish is safe to republish with the same ID.
            const std::string encodedIntent = MessageJson::encode(output);
            bus_.publish(
                MessageSubjects::STRATEGY_INTENTS,
                encodedIntent,
                output.metadata.message_id
            );

            std::size_t activeSignals = 0;
            for (const StrategySignalIntent& strategy : output.strategies)
                activeSignals += strategy.signals.size();

            LG_INFO(
                "service=strategy event=strategy_intents_published timestamp={} strategies={} active_signals={} catchup_days={} message_id={} correlation_id={}",
                output.timestamp,
                output.strategies.size(),
                activeSignals,
                datesToProcess.size(),
                output.metadata.message_id,
                output.metadata.correlation_id
            );

            if (checkpoint_store_) {
                checkpoint_store_->save(target, message.payload, encodedIntent);
                durable_checkpoint_timestamp_ = target;
                LG_INFO(
                    "service=strategy event=strategy_checkpoint_committed timestamp={} checkpoint=latest-state-plus-bounded-market-db",
                    target
                );
            }

            return DurableMessageDisposition::Ack;
        }
        catch (const std::logic_error& error) {
            LG_ALERT(
                "service=strategy event=market_update_rejected disposition=terminate error={}",
                error.what()
            );
            return DurableMessageDisposition::Terminate;
        }
        catch (const std::exception& error) {
            LG_ERROR(
                "service=strategy event=market_update_failed disposition=retry error={}",
                error.what()
            );
            return DurableMessageDisposition::Retry;
        }
    }

public:
    explicit StrategyServiceRuntime(Options options)
        : options_(std::move(options)),
          time_config_(TimeHandlerFactory::loadConfigFromEnvironment()),
          time_handler_(TimeHandlerFactory::create(time_config_)),
          bootstrap_completed_date_(configuredBootstrapCompletedUtcDate(time_config_)),
          bus_(options_.nats_url),
          market_reader_(options_.market_data_db)
    {
        bus_.ensureStream(options_.stream, MessageSubjects::tradingRuntimeSubjects());

        resetRuntimeState();

        if (options_.postgres.empty())
            throw std::invalid_argument(
                "--postgres is required in LIVE strategy mode because the daily checkpoint is the durable processing gate");

        checkpoint_store_ = std::make_unique<StrategyCheckpointStore>(
            options_.postgres,
            checkpointIdentity(options_)
        );
        recoverFromCheckpoint();

        DurableConsumerOptions consumer;
        consumer.stream = options_.stream;
        consumer.durable_name = "strategy-service-market-updates";
        consumer.subject = MessageSubjects::MARKET_DATA_UPDATED;
        consumer.ack_wait_ms = 30000;
        consumer.max_deliver = 20;
        consumer.max_ack_pending = 64;

        market_update_subscription_ = bus_.subscribe(
            consumer,
            [this](const BusMessage& message) { return onMarketDataUpdated(message); }
        );
    }

    ~StrategyServiceRuntime()
    {
        bus_.close(market_update_subscription_);
    }

    void run()
    {
        constexpr auto loopPeriod = std::chrono::milliseconds(1000);
        constexpr std::int64_t fetchTimeoutMs = 100;

        LG_INFO(
            "service=strategy event=service_ready time_source=time_handler stream={} config={} market_database={} market_warmup_days={} market_top_n={} market_data_source={} input_subject={} restart_checkpoint=postgres-latest-intent loop_period_ms={} time_speed={} time_identity={}",
            options_.stream,
            options_.strategy_config,
            options_.market_data_db.string(),
            options_.market_warmup_days,
            options_.market_top_n,
            options_.market_data_source,
            MessageSubjects::MARKET_DATA_UPDATED,
            loopPeriod.count(),
            time_config_.speed,
            time_config_.identity()
        );
        if (bootstrap_completed_date_.has_value()) {
            LG_INFO(
                "service=strategy event=immutable_bootstrap_business_anchor completed_date={} source=simulated_reference",
                *bootstrap_completed_date_
            );
        }
        std::cout.flush();

        bool waitingForNextUtcDayLogged = false;
        Timestamp loggedCompletedDate = 0;

        while (running.load()) {
            const Timestamp newestCompleted = newestCompletedBusinessUtcDate(time_handler_);

            // Once the required BUSINESS date has a durable PostgreSQL checkpoint,
            // Strategy does no more market work until TimeHandler advances to the next
            // business UTC day. No shared/distributed clock participates in this decision.
            if (durable_checkpoint_timestamp_ >= newestCompleted) {
                if (!waitingForNextUtcDayLogged || loggedCompletedDate != newestCompleted) {
                    LG_INFO(
                        "service=strategy event=daily_checkpoint_complete timestamp={} action=wait_until_next_business_utc_day",
                        durable_checkpoint_timestamp_
                    );
                    waitingForNextUtcDayLogged = true;
                    loggedCompletedDate = newestCompleted;
                }

                interruptibleBusinessWaitUntil(
                    time_handler_,
                    computeNextMidnightUTC(time_handler_.getTime())
                );
                continue;
            }

            waitingForNextUtcDayLogged = false;

            // When Strategy is behind its durable checkpoint, drain retained JetStream
            // notifications at TECHNICAL speed.  A 1-second sleep here is incorrect under
            // accelerated business time: at 1500x a large retained backlog can consume
            // multiple simulated days before the latest canonical notification is reached.
            // poll() itself blocks for fetchTimeoutMs when no message is available, so this
            // remains a bounded technical wait rather than a busy-spin.
            bus_.poll(market_update_subscription_, 32, fetchTimeoutMs);
        }

        LG_INFO("service=strategy event=shutdown_requested");
        bus_.flush();
        LG_INFO("service=strategy event=shutdown_complete");
    }
};

} // namespace


// Process entrypoint.

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
