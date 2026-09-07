#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
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
#include "entry_exit_only_rebalance_policy.h"
#include "equal_weight_sizer.h"
#include "nats_jetstream_message_bus.h"
#include "portfolio_risk_engine.h"
#include "rolling_market_state.h"
#include "service_logging.h"
#include "sample_covariance_estimator.h"
#include "threshold_rebalance_policy.h"
#include "transport_subjects.h"
#include "volatility_target_sizer.h"

namespace {

using json = nlohmann::json;
std::atomic<bool> running{true};

void stopHandler(int) { running.store(false); }

struct Options {
    std::string nats_url = "nats://127.0.0.1:4222";
    std::string stream = "ALGOTRADING_RUNTIME";
    std::string portfolio_config = "config/portfolio/pure_rsi_equal_weight.json";
    std::string postgres;
    int poll_timeout_ms = 250;
};

void printUsage()
{
    std::cout
        << "Usage: algotrading_portfolio_risk_service [options]\n"
        << "  --nats-url URL\n"
        << "  --stream NAME\n"
        << "  --portfolio-config PATH\n"
        << "  --postgres CONNECTION_STRING\n"
        << "  --poll-timeout-ms N\n";
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
        } else if (arg == "--portfolio-config") {
            options.portfolio_config = requireValue("--portfolio-config");
        } else if (arg == "--postgres") {
            options.postgres = requireValue("--postgres");
        } else if (arg == "--poll-timeout-ms") {
            options.poll_timeout_ms = std::stoi(requireValue("--poll-timeout-ms"));
        } else {
            throw std::invalid_argument("Unknown option: " + arg);
        }
    }

    if (options.nats_url.empty() || options.stream.empty() || options.portfolio_config.empty())
        throw std::invalid_argument("Portfolio-risk service string options cannot be empty");
    if (options.poll_timeout_ms <= 0)
        throw std::invalid_argument("--poll-timeout-ms must be positive");
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

class PgResult {
private:
    PGresult* result_ = nullptr;

public:
    explicit PgResult(PGresult* result) : result_(result) {}
    ~PgResult() { if (result_) PQclear(result_); }
    PgResult(const PgResult&) = delete;
    PgResult& operator=(const PgResult&) = delete;
    PgResult(PgResult&& other) noexcept : result_(std::exchange(other.result_, nullptr)) {}
    PgResult& operator=(PgResult&& other) noexcept
    {
        if (this != &other) {
            if (result_) PQclear(result_);
            result_ = std::exchange(other.result_, nullptr);
        }
        return *this;
    }
    PGresult* get() const { return result_; }
};

class PortfolioRiskCheckpointStore {
private:
    PGconn* connection_ = nullptr;
    static constexpr const char* STATE_KEY = "portfolio-risk";

    void requireConnection() const
    {
        if (connection_ == nullptr || PQstatus(connection_) != CONNECTION_OK)
            throw std::runtime_error("Portfolio-risk checkpoint PostgreSQL connection is not ready");
    }

    PgResult exec(const std::string& sql, ExecStatusType expected) const
    {
        requireConnection();
        PgResult result(PQexec(connection_, sql.c_str()));
        if (result.get() == nullptr || PQresultStatus(result.get()) != expected)
            throw std::runtime_error(
                "Portfolio-risk checkpoint PostgreSQL query failed: " +
                std::string(connection_ ? PQerrorMessage(connection_) : "unknown PostgreSQL error")
            );
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
        if (result.get() == nullptr || PQresultStatus(result.get()) != expected)
            throw std::runtime_error(
                "Portfolio-risk checkpoint PostgreSQL parameterized query failed: " +
                std::string(connection_ ? PQerrorMessage(connection_) : "unknown PostgreSQL error")
            );
        return result;
    }

    void ensureSchema(const std::string& portfolioConfig)
    {
        exec(
            "CREATE TABLE IF NOT EXISTS portfolio_risk_service_metadata ("
            "state_key TEXT PRIMARY KEY, portfolio_config TEXT NOT NULL)",
            PGRES_COMMAND_OK
        );
        exec(
            "CREATE TABLE IF NOT EXISTS portfolio_risk_market_slice_checkpoint ("
            "state_key TEXT NOT NULL, timestamp BIGINT NOT NULL, payload TEXT NOT NULL, "
            "PRIMARY KEY(state_key, timestamp))",
            PGRES_COMMAND_OK
        );
        // AccountSnapshot is a revision stream, not a single row per timestamp.
        // ExecutionState may publish execution/fill/close snapshots for the same business
        // timestamp. Persist every logical message once and preserve arrival order so
        // recovery reconstructs the same "latest snapshot for timestamp" state.
        exec(
            "CREATE TABLE IF NOT EXISTS portfolio_risk_account_snapshot_checkpoint_v2 ("
            "delivery_sequence BIGSERIAL PRIMARY KEY, "
            "state_key TEXT NOT NULL, timestamp BIGINT NOT NULL, "
            "message_id TEXT NOT NULL, payload TEXT NOT NULL, "
            "UNIQUE(state_key, message_id))",
            PGRES_COMMAND_OK
        );
        exec(
            "CREATE INDEX IF NOT EXISTS portfolio_risk_account_snapshot_checkpoint_v2_state_seq "
            "ON portfolio_risk_account_snapshot_checkpoint_v2(state_key, delivery_sequence)",
            PGRES_COMMAND_OK
        );
        exec(
            "CREATE TABLE IF NOT EXISTS portfolio_risk_decision_checkpoint ("
            "state_key TEXT NOT NULL, timestamp BIGINT NOT NULL, signals_payload TEXT NOT NULL, "
            "decision_payload TEXT NOT NULL, PRIMARY KEY(state_key, timestamp))",
            PGRES_COMMAND_OK
        );
        execParams(
            "INSERT INTO portfolio_risk_service_metadata(state_key, portfolio_config) "
            "VALUES($1, $2) ON CONFLICT(state_key) DO NOTHING",
            {STATE_KEY, portfolioConfig}, PGRES_COMMAND_OK
        );
        const PgResult metadata = execParams(
            "SELECT portfolio_config FROM portfolio_risk_service_metadata WHERE state_key=$1",
            {STATE_KEY}, PGRES_TUPLES_OK
        );
        if (PQntuples(metadata.get()) != 1)
            throw std::runtime_error("Portfolio-risk checkpoint metadata row is missing");
        if (std::string(PQgetvalue(metadata.get(), 0, 0)) != portfolioConfig)
            throw std::runtime_error(
                "Persisted portfolio-risk checkpoint belongs to a different portfolio configuration"
            );
    }

    static Timestamp parseTimestamp(const char* value)
    {
        const unsigned long long parsed = std::stoull(value);
        if (parsed > static_cast<unsigned long long>(std::numeric_limits<Timestamp>::max()))
            throw std::runtime_error("Persisted portfolio-risk checkpoint timestamp is out of range");
        return static_cast<Timestamp>(parsed);
    }

public:
    struct PayloadRow {
        Timestamp timestamp = 0;
        std::string payload;
    };
    struct DecisionRow {
        Timestamp timestamp = 0;
        std::string signals_payload;
        std::string decision_payload;
    };

    PortfolioRiskCheckpointStore(
        const std::string& connectionString,
        const std::string& portfolioConfig
    )
    {
        connection_ = PQconnectdb(connectionString.c_str());
        if (connection_ == nullptr || PQstatus(connection_) != CONNECTION_OK) {
            const std::string error = connection_ ? PQerrorMessage(connection_) : "cannot allocate PGconn";
            if (connection_) {
                PQfinish(connection_);
                connection_ = nullptr;
            }
            throw std::runtime_error("Cannot connect portfolio-risk checkpoint store to PostgreSQL: " + error);
        }
        ensureSchema(portfolioConfig);
    }

    ~PortfolioRiskCheckpointStore() { if (connection_) PQfinish(connection_); }
    PortfolioRiskCheckpointStore(const PortfolioRiskCheckpointStore&) = delete;
    PortfolioRiskCheckpointStore& operator=(const PortfolioRiskCheckpointStore&) = delete;

    std::vector<PayloadRow> loadPayloadRows(const std::string& table) const
    {
        const PgResult result = execParams(
            "SELECT timestamp, payload FROM " + table + " WHERE state_key=$1 ORDER BY timestamp ASC",
            {STATE_KEY}, PGRES_TUPLES_OK
        );
        std::vector<PayloadRow> rows;
        rows.reserve(static_cast<std::size_t>(PQntuples(result.get())));
        for (int row = 0; row < PQntuples(result.get()); ++row) {
            PayloadRow value;
            value.timestamp = parseTimestamp(PQgetvalue(result.get(), row, 0));
            value.payload = PQgetvalue(result.get(), row, 1);
            rows.push_back(std::move(value));
        }
        return rows;
    }

    std::vector<PayloadRow> loadAccountSnapshotRows() const
    {
        const PgResult result = execParams(
            "SELECT timestamp, payload FROM portfolio_risk_account_snapshot_checkpoint_v2 "
            "WHERE state_key=$1 ORDER BY delivery_sequence ASC",
            {STATE_KEY}, PGRES_TUPLES_OK
        );
        std::vector<PayloadRow> rows;
        rows.reserve(static_cast<std::size_t>(PQntuples(result.get())));
        for (int row = 0; row < PQntuples(result.get()); ++row) {
            PayloadRow value;
            value.timestamp = parseTimestamp(PQgetvalue(result.get(), row, 0));
            value.payload = PQgetvalue(result.get(), row, 1);
            rows.push_back(std::move(value));
        }
        return rows;
    }

    std::optional<std::string> accountSnapshotPayloadForMessage(
        const std::string& messageId
    ) const
    {
        const PgResult result = execParams(
            "SELECT payload FROM portfolio_risk_account_snapshot_checkpoint_v2 "
            "WHERE state_key=$1 AND message_id=$2",
            {STATE_KEY, messageId}, PGRES_TUPLES_OK
        );
        if (PQntuples(result.get()) == 0)
            return std::nullopt;
        if (PQntuples(result.get()) != 1)
            throw std::runtime_error("Portfolio-risk account snapshot message id is not unique");
        return std::string(PQgetvalue(result.get(), 0, 0));
    }

    void saveAccountSnapshot(
        Timestamp timestamp,
        const std::string& messageId,
        const std::string& payload
    )
    {
        execParams(
            "INSERT INTO portfolio_risk_account_snapshot_checkpoint_v2("
            "state_key,timestamp,message_id,payload) VALUES($1,$2,$3,$4) "
            "ON CONFLICT(state_key,message_id) DO NOTHING",
            {STATE_KEY, std::to_string(timestamp), messageId, payload},
            PGRES_COMMAND_OK
        );
        const auto persisted = accountSnapshotPayloadForMessage(messageId);
        if (!persisted.has_value() || *persisted != payload)
            throw std::logic_error("Conflicting persisted portfolio-risk account snapshot message");
    }

    std::optional<std::string> payloadFor(
        const std::string& table,
        Timestamp timestamp
    ) const
    {
        const PgResult result = execParams(
            "SELECT payload FROM " + table + " WHERE state_key=$1 AND timestamp=$2",
            {STATE_KEY, std::to_string(timestamp)}, PGRES_TUPLES_OK
        );
        if (PQntuples(result.get()) == 0)
            return std::nullopt;
        if (PQntuples(result.get()) != 1)
            throw std::runtime_error("Portfolio-risk checkpoint timestamp is not unique");
        return std::string(PQgetvalue(result.get(), 0, 0));
    }

    void savePayload(
        const std::string& table,
        Timestamp timestamp,
        const std::string& payload
    )
    {
        execParams(
            "INSERT INTO " + table + "(state_key, timestamp, payload) VALUES($1,$2,$3) "
            "ON CONFLICT(state_key,timestamp) DO NOTHING",
            {STATE_KEY, std::to_string(timestamp), payload}, PGRES_COMMAND_OK
        );
        const auto persisted = payloadFor(table, timestamp);
        if (!persisted.has_value() || *persisted != payload)
            throw std::logic_error("Conflicting persisted portfolio-risk checkpoint payload");
    }

    std::optional<DecisionRow> decisionFor(Timestamp timestamp) const
    {
        const PgResult result = execParams(
            "SELECT signals_payload, decision_payload FROM portfolio_risk_decision_checkpoint "
            "WHERE state_key=$1 AND timestamp=$2",
            {STATE_KEY, std::to_string(timestamp)}, PGRES_TUPLES_OK
        );
        if (PQntuples(result.get()) == 0)
            return std::nullopt;
        if (PQntuples(result.get()) != 1)
            throw std::runtime_error("Portfolio-risk decision checkpoint timestamp is not unique");
        DecisionRow row;
        row.timestamp = timestamp;
        row.signals_payload = PQgetvalue(result.get(), 0, 0);
        row.decision_payload = PQgetvalue(result.get(), 0, 1);
        return row;
    }

    Timestamp latestDecisionTimestamp() const
    {
        const PgResult result = execParams(
            "SELECT COALESCE(MAX(timestamp),0) FROM portfolio_risk_decision_checkpoint WHERE state_key=$1",
            {STATE_KEY}, PGRES_TUPLES_OK
        );
        if (PQntuples(result.get()) != 1)
            throw std::runtime_error("Portfolio-risk decision checkpoint aggregate failed");
        return parseTimestamp(PQgetvalue(result.get(), 0, 0));
    }

    void saveDecision(
        Timestamp timestamp,
        const std::string& signalsPayload,
        const std::string& decisionPayload
    )
    {
        execParams(
            "INSERT INTO portfolio_risk_decision_checkpoint("
            "state_key,timestamp,signals_payload,decision_payload) VALUES($1,$2,$3,$4) "
            "ON CONFLICT(state_key,timestamp) DO NOTHING",
            {STATE_KEY, std::to_string(timestamp), signalsPayload, decisionPayload},
            PGRES_COMMAND_OK
        );
        const auto persisted = decisionFor(timestamp);
        if (!persisted.has_value() || persisted->signals_payload != signalsPayload ||
            persisted->decision_payload != decisionPayload)
            throw std::logic_error("Conflicting persisted portfolio-risk decision checkpoint");
    }
};

std::unique_ptr<PortfolioSizer> makeSizer(const json& value)
{
    const std::string type = value.at("type").get<std::string>();
    if (type == "equal_weight") {
        return std::make_unique<EqualWeightSizer>(
            value.at("weight_per_full_signal").get<double>()
        );
    }

    if (type == "volatility_target") {
        return std::make_unique<VolatilityTargetSizer>(
            std::make_unique<SampleCovarianceEstimator>(
                value.at("covariance_lookback").get<std::size_t>(),
                value.at("periods_per_year").get<double>()
            ),
            value.at("target_volatility").get<double>()
        );
    }

    throw std::invalid_argument("Unsupported portfolio sizer type: " + type);
}

std::unique_ptr<RebalancePolicy> makeRebalancePolicy(const json& value)
{
    const std::string type = value.at("type").get<std::string>();
    if (type == "entry_exit_only")
        return std::make_unique<EntryExitOnlyRebalancePolicy>();
    if (type == "threshold")
        return std::make_unique<ThresholdRebalancePolicy>(value.at("threshold").get<double>());
    throw std::invalid_argument("Unsupported rebalance policy type: " + type);
}

std::vector<PortfolioRiskStrategyConfig> loadPortfolioConfig(const std::string& path)
{
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("Cannot open portfolio-risk config: " + path);

    json root;
    input >> root;

    std::vector<PortfolioRiskStrategyConfig> strategies;
    std::unordered_set<StrategyID> ids;

    for (const json& value : root.at("strategies")) {
        const unsigned long parsedId = value.at("id").get<unsigned long>();
        if (parsedId == 0 || parsedId > static_cast<unsigned long>(std::numeric_limits<StrategyID>::max()))
            throw std::invalid_argument("Portfolio-risk strategy id is out of range");

        const StrategyID strategyId = static_cast<StrategyID>(parsedId);
        if (!ids.insert(strategyId).second)
            throw std::invalid_argument("Duplicate strategy id in portfolio-risk config");

        const json& risk = value.at("risk");
        strategies.emplace_back(
            strategyId,
            value.at("name").get<std::string>(),
            value.at("allocation_weight").get<double>(),
            makeSizer(value.at("sizer")),
            RiskConstraints(
                risk.at("max_gross_leverage").get<double>(),
                risk.at("max_asset_weight").get<double>()
            ),
            makeRebalancePolicy(value.at("rebalance"))
        );
    }

    if (strategies.empty())
        throw std::invalid_argument("Portfolio-risk config must contain at least one strategy");
    return strategies;
}

std::string decisionMessageId(Timestamp timestamp)
{
    return "portfolio-decision:" + std::to_string(timestamp);
}

class PortfolioRiskServiceRuntime {
private:
    const Options options_;
    NatsJetStreamMessageBus bus_;
    std::unique_ptr<PortfolioRiskCheckpointStore> checkpoint_store_;
    RollingMarketState market_state_;
    std::unique_ptr<PortfolioRiskEngine> engine_;
    std::map<Timestamp, AccountSnapshot> account_snapshots_;

    DurableMessageBus::SubscriptionID market_subscription_ = 0;
    DurableMessageBus::SubscriptionID account_subscription_ = 0;
    DurableMessageBus::SubscriptionID strategy_subscription_ = 0;

    void pruneAccountSnapshots()
    {
        while (account_snapshots_.size() > 128)
            account_snapshots_.erase(account_snapshots_.begin());
    }


    void resetRuntimeState()
    {
        market_state_ = RollingMarketState{};
        account_snapshots_.clear();
        engine_ = std::make_unique<PortfolioRiskEngine>(loadPortfolioConfig(options_.portfolio_config));
    }

    void recoverFromCheckpoint()
    {
        if (!checkpoint_store_)
            return;

        resetRuntimeState();
        const auto marketRows = checkpoint_store_->loadPayloadRows(
            "portfolio_risk_market_slice_checkpoint"
        );
        for (const auto& row : marketRows) {
            MarketSliceSnapshot slice = ContractJsonCodec::decodeMarketSliceSnapshot(row.payload);
            if (slice.timestamp != row.timestamp)
                throw std::logic_error("Persisted portfolio-risk market timestamp mismatch");
            if (!market_state_.append(slice))
                throw std::logic_error("Duplicate market slice inside portfolio-risk checkpoint");
        }

        const auto accountRows = checkpoint_store_->loadAccountSnapshotRows();
        for (const auto& row : accountRows) {
            AccountSnapshot snapshot = ContractJsonCodec::decodeAccountSnapshot(row.payload);
            if (snapshot.timestamp != row.timestamp)
                throw std::logic_error("Persisted portfolio-risk account timestamp mismatch");
            account_snapshots_[row.timestamp] = std::move(snapshot);
        }
        pruneAccountSnapshots();

        const Timestamp lastDecision = checkpoint_store_->latestDecisionTimestamp();
        engine_->restoreLastTimestamp(lastDecision);

        LG_INFO(
            "service=portfolio-risk event=portfolio_risk_recovery_completed recovered_market_slices={} recovered_account_snapshots={} last_decision_timestamp={}",
            marketRows.size(), accountRows.size(), lastDecision
        );
    }

    DurableMessageDisposition onMarketSlice(const BusMessage& message)
    {
        try {
            const MarketSliceSnapshot slice = ContractJsonCodec::decodeMarketSliceSnapshot(message.payload);
            if (slice.timestamp == 0 || slice.bars.empty()) {
                LG_WARN(
                    "service=portfolio-risk event=market_slice_terminated reason=invalid_slice timestamp={} bars={} message_id={}",
                    slice.timestamp,
                    slice.bars.size(),
                    slice.metadata.message_id
                );
                return DurableMessageDisposition::Terminate;
            }

            if (checkpoint_store_) {
                const auto persisted = checkpoint_store_->payloadFor(
                    "portfolio_risk_market_slice_checkpoint",
                    slice.timestamp
                );
                if (persisted.has_value()) {
                    if (*persisted != message.payload) {
                        LG_ALERT(
                            "service=portfolio-risk event=market_slice_checkpoint_conflict timestamp={} disposition=terminate",
                            slice.timestamp
                        );
                        return DurableMessageDisposition::Terminate;
                    }
                    if (market_state_.empty() || market_state_.latestTimestamp() < slice.timestamp)
                        recoverFromCheckpoint();
                    LG_INFO(
                        "service=portfolio-risk event=market_slice_checkpoint_duplicate timestamp={} disposition=ack",
                        slice.timestamp
                    );
                    return DurableMessageDisposition::Ack;
                }
                if (!market_state_.empty() && slice.timestamp <= market_state_.latestTimestamp()) {
                    LG_WARN(
                        "service=portfolio-risk event=volatile_market_state_ahead_of_checkpoint timestamp={} latest_runtime_timestamp={} action=recover",
                        slice.timestamp,
                        market_state_.latestTimestamp()
                    );
                    recoverFromCheckpoint();
                }
            }

            const bool appended = market_state_.append(slice);
            if (checkpoint_store_ && appended)
                checkpoint_store_->savePayload(
                    "portfolio_risk_market_slice_checkpoint",
                    slice.timestamp,
                    message.payload
                );

            LG_DEBUG(
                "service=portfolio-risk event=market_slice_buffered timestamp={} bars={} appended={} message_id={}",
                slice.timestamp,
                slice.bars.size(),
                appended,
                slice.metadata.message_id
            );
            return DurableMessageDisposition::Ack;
        }
        catch (const std::logic_error& error) {
            LG_WARN("service=portfolio-risk event=market_slice_rejected disposition=terminate error={}", error.what());
            return DurableMessageDisposition::Terminate;
        }
        catch (const std::exception& error) {
            LG_ERROR("service=portfolio-risk event=market_slice_failed disposition=retry error={}", error.what());
            return DurableMessageDisposition::Retry;
        }
    }

    DurableMessageDisposition onAccountSnapshot(const BusMessage& message)
    {
        try {
            AccountSnapshot snapshot = ContractJsonCodec::decodeAccountSnapshot(message.payload);
            if (!std::isfinite(snapshot.cash))
                return DurableMessageDisposition::Terminate;

            for (const auto& [coin, quantity] : snapshot.positions) {
                if (coin.empty() || !std::isfinite(quantity))
                    return DurableMessageDisposition::Terminate;
            }
            for (const auto& [strategyId, positions] : snapshot.strategy_positions) {
                if (strategyId == 0)
                    return DurableMessageDisposition::Terminate;
                for (const auto& [coin, quantity] : positions) {
                    if (coin.empty() || !std::isfinite(quantity))
                        return DurableMessageDisposition::Terminate;
                }
            }

            const Timestamp snapshotTimestamp = snapshot.timestamp;
            const double cash = snapshot.cash;
            const std::size_t physicalPositions = snapshot.positions.size();
            const std::size_t strategyPositionSets = snapshot.strategy_positions.size();
            const std::string snapshotMessageId = snapshot.metadata.message_id;

            if (snapshot.metadata.message_id.empty()) {
                LG_WARN(
                    "service=portfolio-risk event=account_snapshot_terminated reason=missing_message_id timestamp={}",
                    snapshot.timestamp
                );
                return DurableMessageDisposition::Terminate;
            }

            if (checkpoint_store_) {
                const auto persisted = checkpoint_store_->accountSnapshotPayloadForMessage(
                    snapshot.metadata.message_id
                );
                if (persisted.has_value()) {
                    if (*persisted != message.payload) {
                        LG_ALERT(
                            "service=portfolio-risk event=account_snapshot_checkpoint_conflict "
                            "timestamp={} message_id={} disposition=terminate",
                            snapshot.timestamp,
                            snapshot.metadata.message_id
                        );
                        return DurableMessageDisposition::Terminate;
                    }

                    // The logical message was already applied before ACK/crash. Recovery has
                    // replayed all checkpoint rows in delivery order, so reapplying an older
                    // redelivery here could roll the in-memory timestamp back to stale cash/
                    // positions. ACK it without mutating runtime state.
                    LG_INFO(
                        "service=portfolio-risk event=account_snapshot_checkpoint_duplicate "
                        "timestamp={} message_id={} disposition=ack",
                        snapshot.timestamp,
                        snapshot.metadata.message_id
                    );
                    return DurableMessageDisposition::Ack;
                }

                checkpoint_store_->saveAccountSnapshot(
                    snapshot.timestamp,
                    snapshot.metadata.message_id,
                    message.payload
                );
            }

            account_snapshots_[snapshot.timestamp] = std::move(snapshot);
            pruneAccountSnapshots();
            LG_INFO(
                "service=portfolio-risk event=account_snapshot_buffered timestamp={} cash={} physical_positions={} strategy_position_sets={} buffered_snapshots={} message_id={}",
                snapshotTimestamp,
                cash,
                physicalPositions,
                strategyPositionSets,
                account_snapshots_.size(),
                snapshotMessageId
            );
            return DurableMessageDisposition::Ack;
        }
        catch (const std::exception& error) {
            LG_ERROR("service=portfolio-risk event=account_snapshot_failed disposition=retry error={}", error.what());
            return DurableMessageDisposition::Retry;
        }
    }

    DurableMessageDisposition onStrategyIntents(const BusMessage& message)
    {
        try {
            StrategyIntentBatch signals = ContractJsonCodec::decodeStrategyIntentBatch(message.payload);
            if (signals.timestamp == 0) {
                LG_WARN("service=portfolio-risk event=strategy_intents_terminated reason=zero_timestamp");
                return DurableMessageDisposition::Terminate;
            }
            if (checkpoint_store_) {
                const auto persistedDecision = checkpoint_store_->decisionFor(signals.timestamp);
                if (persistedDecision.has_value()) {
                    if (persistedDecision->signals_payload != message.payload) {
                        LG_ALERT(
                            "service=portfolio-risk event=decision_checkpoint_conflict timestamp={} disposition=terminate",
                            signals.timestamp
                        );
                        return DurableMessageDisposition::Terminate;
                    }
                    LG_INFO(
                        "service=portfolio-risk event=strategy_intents_checkpoint_duplicate timestamp={} disposition=ack",
                        signals.timestamp
                    );
                    return DurableMessageDisposition::Ack;
                }

                if (signals.timestamp <= engine_->lastTimestamp()) {
                    LG_WARN(
                        "service=portfolio-risk event=volatile_decision_state_ahead_of_checkpoint timestamp={} last_runtime_timestamp={} action=recover",
                        signals.timestamp,
                        engine_->lastTimestamp()
                    );
                    recoverFromCheckpoint();
                }
            }

            if (signals.timestamp <= engine_->lastTimestamp()) {
                LG_INFO(
                    "service=portfolio-risk event=strategy_intents_duplicate timestamp={} last_timestamp={} disposition=ack",
                    signals.timestamp,
                    engine_->lastTimestamp()
                );
                return DurableMessageDisposition::Ack;
            }

            if (market_state_.marketData().find(signals.timestamp) == market_state_.marketData().end()) {
                LG_DEBUG(
                    "service=portfolio-risk event=strategy_intents_waiting timestamp={} missing=market_slice disposition=retry",
                    signals.timestamp
                );
                return DurableMessageDisposition::Retry;
            }

            const auto accountIt = account_snapshots_.find(signals.timestamp);
            if (accountIt == account_snapshots_.end()) {
                LG_DEBUG(
                    "service=portfolio-risk event=strategy_intents_waiting timestamp={} missing=account_snapshot disposition=retry",
                    signals.timestamp
                );
                return DurableMessageDisposition::Retry;
            }

            DecisionBatch output = engine_->onSignals(
                signals,
                market_state_.marketData(),
                accountIt->second
            );

            output.metadata.schema_version = 1;
            output.metadata.message_id = decisionMessageId(signals.timestamp);
            output.metadata.correlation_id = !signals.metadata.correlation_id.empty()
                ? signals.metadata.correlation_id
                : signals.metadata.message_id;
            output.metadata.produced_at = signals.timestamp;

            const std::string encodedDecision = ContractJsonCodec::encode(output);
            bus_.publish(
                TransportSubjects::DECISION_BATCH,
                encodedDecision,
                output.metadata.message_id
            );

            if (checkpoint_store_)
                checkpoint_store_->saveDecision(
                    signals.timestamp,
                    message.payload,
                    encodedDecision
                );

            std::size_t decisions = 0;
            for (const StrategyDecisionIntent& strategy : output.strategies)
                decisions += strategy.decisions.size();

            LG_INFO(
                "service=portfolio-risk event=decision_published timestamp={} strategies={} decisions={} reference_cash={} message_id={} correlation_id={}",
                output.decision_timestamp,
                output.strategies.size(),
                decisions,
                accountIt->second.cash,
                output.metadata.message_id,
                output.metadata.correlation_id
            );

            return DurableMessageDisposition::Ack;
        }
        catch (const std::logic_error& error) {
            LG_WARN("service=portfolio-risk event=strategy_intents_rejected disposition=terminate error={}", error.what());
            return DurableMessageDisposition::Terminate;
        }
        catch (const std::exception& error) {
            LG_ERROR("service=portfolio-risk event=strategy_intents_failed disposition=retry error={}", error.what());
            return DurableMessageDisposition::Retry;
        }
    }

    DurableConsumerOptions consumer(const std::string& durable, const std::string& subject) const
    {
        DurableConsumerOptions result;
        result.stream = options_.stream;
        result.durable_name = durable;
        result.subject = subject;
        result.ack_wait_ms = 30000;
        result.max_deliver = 20;
        result.max_ack_pending = 128;
        return result;
    }

public:
    explicit PortfolioRiskServiceRuntime(Options options)
        : options_(std::move(options)),
          bus_(options_.nats_url)
    {
        resetRuntimeState();
        if (!options_.postgres.empty()) {
            checkpoint_store_ = std::make_unique<PortfolioRiskCheckpointStore>(
                options_.postgres,
                readTextFile(options_.portfolio_config)
            );
            recoverFromCheckpoint();
        } else {
            LG_WARN("service=portfolio-risk event=restart_checkpoint_disabled reason=postgres_not_configured");
        }

        bus_.ensureStream(options_.stream, TransportSubjects::runtimeSubjects());

        market_subscription_ = bus_.subscribe(
            consumer("portfolio-risk-market-slices", TransportSubjects::MARKET_SLICE_SNAPSHOT),
            [this](const BusMessage& message) { return onMarketSlice(message); }
        );
        account_subscription_ = bus_.subscribe(
            consumer("portfolio-risk-account-snapshots", TransportSubjects::ACCOUNT_SNAPSHOT),
            [this](const BusMessage& message) { return onAccountSnapshot(message); }
        );
        strategy_subscription_ = bus_.subscribe(
            consumer("portfolio-risk-strategy-intents", TransportSubjects::STRATEGY_INTENTS),
            [this](const BusMessage& message) { return onStrategyIntents(message); }
        );
    }

    ~PortfolioRiskServiceRuntime()
    {
        bus_.close(strategy_subscription_);
        bus_.close(account_subscription_);
        bus_.close(market_subscription_);
    }

    void run()
    {
        LG_INFO(
            "service=portfolio-risk event=service_ready stream={} config={} restart_checkpoint={} poll_timeout_ms={}",
            options_.stream,
            options_.portfolio_config,
            checkpoint_store_ ? "postgres" : "disabled",
            options_.poll_timeout_ms
        );

        while (running.load()) {
            // Market/account state is polled first so a signal delivery can normally be
            // completed immediately. If transport ordering differs, Retry is safe.
            bus_.poll(market_subscription_, 32, options_.poll_timeout_ms);
            bus_.poll(account_subscription_, 32, options_.poll_timeout_ms);
            bus_.poll(strategy_subscription_, 32, options_.poll_timeout_ms);
        }

        LG_INFO("service=portfolio-risk event=shutdown_requested");
        bus_.flush();
        LG_INFO("service=portfolio-risk event=shutdown_complete");
    }
};

} // namespace

int main(int argc, char** argv)
{
    ServiceLogging::setup("portfolio-risk");

    try {
        std::signal(SIGINT, stopHandler);
        std::signal(SIGTERM, stopHandler);
        PortfolioRiskServiceRuntime runtime(parseOptions(argc, argv));
        runtime.run();
        return 0;
    }
    catch (const std::exception& error) {
        LG_ALERT("service=portfolio-risk event=fatal error={}", error.what());
        return 1;
    }
}
