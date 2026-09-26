#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
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
#include "contract_json_codec.h"
#include "entry_exit_only_rebalance_policy.h"
#include "equal_weight_sizer.h"
#include "nats_jetstream_message_bus.h"
#include "portfolio_risk_engine.h"
#include "sample_covariance_estimator.h"
#include "service_logging.h"
#include "threshold_rebalance_policy.h"
#include "time_handler_factory.h"
#include "time_utils.h"
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
    std::filesystem::path market_data_db = "storage/databases/database.db";
    unsigned int market_history_buffer_days = 5;
    std::string postgres;
};

void printUsage()
{
    std::cout
        << "Usage: algotrading_portfolio_risk_service [options]\n"
        << "  --nats-url URL\n"
        << "  --stream NAME\n"
        << "  --portfolio-config PATH\n"
        << "  --market-data-db PATH              canonical SQLite market database\n"
        << "  --market-history-buffer-days N     extra calendar-day buffer for VolTarget, default 5\n"
        << "  --postgres CONNECTION_STRING\n"
        << "  Business time is provided by TimeHandler; technical polling remains real-time.\n";
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
        } else if (arg == "--market-data-db") {
            options.market_data_db = requireValue("--market-data-db");
        } else if (arg == "--market-history-buffer-days") {
            options.market_history_buffer_days = static_cast<unsigned int>(
                std::stoul(requireValue("--market-history-buffer-days")));
        } else if (arg == "--postgres") {
            options.postgres = requireValue("--postgres");
        } else {
            throw std::invalid_argument("Unknown option: " + arg);
        }
    }

    if (options.nats_url.empty() || options.stream.empty() || options.portfolio_config.empty())
        throw std::invalid_argument("Portfolio-risk required string options cannot be empty");
    if (options.market_data_db.empty())
        throw std::invalid_argument("--market-data-db cannot be empty");
    if (!std::filesystem::exists(options.market_data_db))
        throw std::invalid_argument("Canonical market-data SQLite database does not exist");
    if (options.postgres.empty())
        throw std::invalid_argument(
            "--postgres is required in LIVE portfolio-risk mode because the daily checkpoint is the durable processing gate");
    return options;
}

std::string readTextFile(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Cannot open file: " + path);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

Timestamp newestCompletedBusinessUtcDate(const TimeHandler& timeHandler)
{
    const auto todayUtc = getCurrentUtcDate(timeHandler.getTime());
    return static_cast<Timestamp>(toYYYYMMDD(getPreviousDayDate(todayUtc)));
}


std::optional<Timestamp> configuredReplayBootstrapCompletedUtcDate(const TimeHandlerConfig& config)
{
    const char* simulatedReference = std::getenv(TimeHandlerFactory::SIMULATED_REFERENCE_ENV);
    if (!simulatedReference || *simulatedReference == '\0')
        return std::nullopt;

    const auto referenceUtcDate = getCurrentUtcDate(config.simulated_reference_utc);
    return static_cast<Timestamp>(toYYYYMMDD(getPreviousDayDate(referenceUtcDate)));
}

void interruptibleSleepFor(std::chrono::steady_clock::duration duration)
{
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (running.load()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            return;
        const auto remaining = deadline - now;
        const auto chunk = std::min(
            std::chrono::duration_cast<std::chrono::milliseconds>(remaining),
            std::chrono::milliseconds(100));
        if (chunk.count() > 0)
            std::this_thread::sleep_for(chunk);
    }
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

unsigned int requiredMarketHistoryDays(
    const std::string& path,
    unsigned int bufferDays)
{
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("Cannot open portfolio-risk config: " + path);

    json root;
    input >> root;

    std::size_t maxLookbackReturns = 0;
    for (const json& value : root.at("strategies")) {
        const json& sizer = value.at("sizer");
        if (sizer.at("type").get<std::string>() == "volatility_target")
            maxLookbackReturns = std::max(
                maxLookbackReturns,
                sizer.at("covariance_lookback").get<std::size_t>());
    }

    if (maxLookbackReturns == 0)
        return 1; // EqualWeight only needs today's close for equity/rebalance pricing.

    const std::size_t total = maxLookbackReturns + 1U + bufferDays;
    if (total > static_cast<std::size_t>(std::numeric_limits<unsigned int>::max()))
        throw std::invalid_argument("Portfolio-risk market history requirement is too large");
    return static_cast<unsigned int>(total);
}

std::string checkpointIdentity(
    const Options& options,
    unsigned int requiredHistoryDays)
{
    return readTextFile(options.portfolio_config) +
        "\nmarket_data_mode=canonical-sqlite-v1" +
        "\nmarket_history_days=" + std::to_string(requiredHistoryDays) +
        "\nmarket_history_buffer_days=" + std::to_string(options.market_history_buffer_days);
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
    static constexpr const char* STATE_KEY = "portfolio-risk-live-sqlite-v1";

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
                std::string(connection_ ? PQerrorMessage(connection_) : "unknown PostgreSQL error"));
        return result;
    }

    PgResult execParams(
        const std::string& sql,
        const std::vector<std::string>& parameters,
        ExecStatusType expected) const
    {
        requireConnection();
        std::vector<const char*> values;
        values.reserve(parameters.size());
        for (const std::string& parameter : parameters)
            values.push_back(parameter.c_str());

        PgResult result(PQexecParams(
            connection_, sql.c_str(), static_cast<int>(values.size()), nullptr,
            values.data(), nullptr, nullptr, 0));
        if (result.get() == nullptr || PQresultStatus(result.get()) != expected)
            throw std::runtime_error(
                "Portfolio-risk checkpoint PostgreSQL parameterized query failed: " +
                std::string(connection_ ? PQerrorMessage(connection_) : "unknown PostgreSQL error"));
        return result;
    }

    static Timestamp parseTimestamp(const char* value)
    {
        const unsigned long long parsed = std::stoull(value);
        if (parsed > static_cast<unsigned long long>(std::numeric_limits<Timestamp>::max()))
            throw std::runtime_error("Persisted portfolio-risk timestamp is out of range");
        return static_cast<Timestamp>(parsed);
    }

    void ensureSchema(const std::string& identity)
    {
        exec(
            "CREATE TABLE IF NOT EXISTS portfolio_risk_service_metadata ("
            "state_key TEXT PRIMARY KEY, portfolio_config TEXT NOT NULL)",
            PGRES_COMMAND_OK);

        // New LIVE tables. Old market-slice/account history tables are intentionally
        // left untouched and are not read by this path.
        exec(
            "CREATE TABLE IF NOT EXISTS portfolio_risk_live_account_checkpoint ("
            "state_key TEXT NOT NULL, timestamp BIGINT NOT NULL, "
            "message_id TEXT NOT NULL, payload TEXT NOT NULL, "
            "PRIMARY KEY(state_key,timestamp))",
            PGRES_COMMAND_OK);

        exec(
            "CREATE TABLE IF NOT EXISTS portfolio_risk_live_decision_checkpoint ("
            "state_key TEXT NOT NULL, timestamp BIGINT NOT NULL, "
            "signals_payload TEXT NOT NULL, account_payload TEXT NOT NULL, "
            "decision_payload TEXT NOT NULL, PRIMARY KEY(state_key, timestamp))",
            PGRES_COMMAND_OK);

        execParams(
            "INSERT INTO portfolio_risk_service_metadata(state_key, portfolio_config) "
            "VALUES($1,$2) ON CONFLICT(state_key) DO NOTHING",
            {STATE_KEY, identity}, PGRES_COMMAND_OK);

        const PgResult metadata = execParams(
            "SELECT portfolio_config FROM portfolio_risk_service_metadata WHERE state_key=$1",
            {STATE_KEY}, PGRES_TUPLES_OK);
        if (PQntuples(metadata.get()) != 1)
            throw std::runtime_error("Portfolio-risk LIVE checkpoint metadata row is missing");
        if (std::string(PQgetvalue(metadata.get(), 0, 0)) != identity)
            throw std::runtime_error(
                "Persisted portfolio-risk LIVE checkpoint belongs to a different portfolio/config identity");
    }

public:
    struct AccountRow {
        Timestamp timestamp = 0;
        std::string message_id;
        std::string payload;
    };
    struct DecisionRow {
        Timestamp timestamp = 0;
        std::string signals_payload;
        std::string account_payload;
        std::string decision_payload;
    };

    PortfolioRiskCheckpointStore(const std::string& connectionString, const std::string& identity)
    {
        connection_ = PQconnectdb(connectionString.c_str());
        if (connection_ == nullptr || PQstatus(connection_) != CONNECTION_OK) {
            const std::string error = connection_ ? PQerrorMessage(connection_) : "cannot allocate PGconn";
            if (connection_) { PQfinish(connection_); connection_ = nullptr; }
            throw std::runtime_error("Cannot connect portfolio-risk checkpoint store to PostgreSQL: " + error);
        }
        ensureSchema(identity);
    }

    ~PortfolioRiskCheckpointStore() { if (connection_) PQfinish(connection_); }
    PortfolioRiskCheckpointStore(const PortfolioRiskCheckpointStore&) = delete;
    PortfolioRiskCheckpointStore& operator=(const PortfolioRiskCheckpointStore&) = delete;

    std::optional<AccountRow> accountFor(Timestamp timestamp) const
    {
        const PgResult result = execParams(
            "SELECT message_id,payload FROM portfolio_risk_live_account_checkpoint "
            "WHERE state_key=$1 AND timestamp=$2",
            {STATE_KEY, std::to_string(timestamp)}, PGRES_TUPLES_OK);
        if (PQntuples(result.get()) == 0)
            return std::nullopt;
        if (PQntuples(result.get()) != 1)
            throw std::runtime_error("Portfolio-risk LIVE account checkpoint timestamp is not unique");
        AccountRow row;
        row.timestamp = timestamp;
        row.message_id = PQgetvalue(result.get(), 0, 0);
        row.payload = PQgetvalue(result.get(), 0, 1);
        return row;
    }

    void saveAccount(Timestamp timestamp, const std::string& messageId, const std::string& payload)
    {
        // One deterministic account reference per decision day. Never replace the
        // already selected daily account snapshot with a later message for the same T.
        execParams(
            "INSERT INTO portfolio_risk_live_account_checkpoint(state_key,timestamp,message_id,payload) "
            "VALUES($1,$2,$3,$4) ON CONFLICT(state_key,timestamp) DO NOTHING",
            {STATE_KEY, std::to_string(timestamp), messageId, payload}, PGRES_COMMAND_OK);

        const auto persisted = accountFor(timestamp);
        if (!persisted.has_value() || persisted->message_id != messageId || persisted->payload != payload)
            throw std::logic_error(
                "Conflicting portfolio-risk LIVE account snapshot for the same decision date");

        // Account snapshots are only a small join buffer between ExecutionState and
        // PortfolioRisk. Keep a bounded durable tail rather than an unbounded history.
        execParams(
            "DELETE FROM portfolio_risk_live_account_checkpoint WHERE state_key=$1 AND timestamp NOT IN ("
            "SELECT timestamp FROM portfolio_risk_live_account_checkpoint WHERE state_key=$1 "
            "ORDER BY timestamp DESC LIMIT 8)",
            {STATE_KEY}, PGRES_COMMAND_OK);
    }

    std::optional<DecisionRow> decisionFor(Timestamp timestamp) const
    {
        const PgResult result = execParams(
            "SELECT signals_payload,account_payload,decision_payload "
            "FROM portfolio_risk_live_decision_checkpoint WHERE state_key=$1 AND timestamp=$2",
            {STATE_KEY, std::to_string(timestamp)}, PGRES_TUPLES_OK);
        if (PQntuples(result.get()) == 0)
            return std::nullopt;
        if (PQntuples(result.get()) != 1)
            throw std::runtime_error("Portfolio-risk LIVE decision checkpoint timestamp is not unique");
        DecisionRow row;
        row.timestamp = timestamp;
        row.signals_payload = PQgetvalue(result.get(), 0, 0);
        row.account_payload = PQgetvalue(result.get(), 0, 1);
        row.decision_payload = PQgetvalue(result.get(), 0, 2);
        return row;
    }

    Timestamp latestDecisionTimestamp() const
    {
        const PgResult result = execParams(
            "SELECT COALESCE(MAX(timestamp),0) FROM portfolio_risk_live_decision_checkpoint WHERE state_key=$1",
            {STATE_KEY}, PGRES_TUPLES_OK);
        if (PQntuples(result.get()) != 1)
            throw std::runtime_error("Portfolio-risk LIVE decision checkpoint aggregate failed");
        return parseTimestamp(PQgetvalue(result.get(), 0, 0));
    }

    void saveDecision(
        Timestamp timestamp,
        const std::string& signalsPayload,
        const std::string& accountPayload,
        const std::string& decisionPayload)
    {
        execParams(
            "INSERT INTO portfolio_risk_live_decision_checkpoint("
            "state_key,timestamp,signals_payload,account_payload,decision_payload) "
            "VALUES($1,$2,$3,$4,$5) ON CONFLICT(state_key,timestamp) DO NOTHING",
            {STATE_KEY, std::to_string(timestamp), signalsPayload, accountPayload, decisionPayload},
            PGRES_COMMAND_OK);

        const auto persisted = decisionFor(timestamp);
        if (!persisted.has_value() || persisted->signals_payload != signalsPayload ||
            persisted->account_payload != accountPayload || persisted->decision_payload != decisionPayload)
            throw std::logic_error("Conflicting persisted portfolio-risk LIVE decision checkpoint");
    }
};

std::unique_ptr<PortfolioSizer> makeSizer(const json& value)
{
    const std::string type = value.at("type").get<std::string>();
    if (type == "equal_weight")
        return std::make_unique<EqualWeightSizer>(value.at("weight_per_full_signal").get<double>());
    if (type == "volatility_target") {
        return std::make_unique<VolatilityTargetSizer>(
            std::make_unique<SampleCovarianceEstimator>(
                value.at("covariance_lookback").get<std::size_t>(),
                value.at("periods_per_year").get<double>()),
            value.at("target_volatility").get<double>());
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
                risk.at("max_asset_weight").get<double>()),
            makeRebalancePolicy(value.at("rebalance")));
    }

    if (strategies.empty())
        throw std::invalid_argument("Portfolio-risk config must contain at least one strategy");
    return strategies;
}

std::string decisionMessageId(Timestamp timestamp)
{
    return "portfolio-decision:" + std::to_string(timestamp);
}

std::set<Coin> requiredSymbols(const StrategyIntentBatch& signals, const AccountSnapshot& account)
{
    std::set<Coin> symbols;
    for (const StrategySignalIntent& strategy : signals.strategies) {
        for (const auto& [coin, signal] : strategy.signals) {
            (void)signal;
            symbols.insert(coin);
        }
    }
    for (const auto& [coin, quantity] : account.positions) {
        (void)quantity;
        symbols.insert(coin);
    }
    for (const auto& [strategyId, positions] : account.strategy_positions) {
        (void)strategyId;
        for (const auto& [coin, quantity] : positions) {
            (void)quantity;
            symbols.insert(coin);
        }
    }
    return symbols;
}

std::size_t marketRowCount(const MarketData& marketData)
{
    std::size_t rows = 0;
    for (const auto& [timestamp, bars] : marketData) {
        (void)timestamp;
        rows += bars.size();
    }
    return rows;
}

class PortfolioRiskServiceRuntime {
private:
    const Options options_;
    const unsigned int required_history_days_;
    const TimeHandlerConfig time_config_;
    const TimeHandler time_handler_;
    const std::optional<Timestamp> replay_bootstrap_completed_date_;
    NatsJetStreamMessageBus bus_;
    CanonicalMarketDataReader market_reader_;
    std::unique_ptr<PortfolioRiskCheckpointStore> checkpoint_store_;
    std::unique_ptr<PortfolioRiskEngine> engine_;
    Timestamp durable_checkpoint_timestamp_ = 0;

    DurableMessageBus::SubscriptionID account_subscription_ = 0;
    DurableMessageBus::SubscriptionID strategy_subscription_ = 0;

    void resetEngine()
    {
        engine_ = std::make_unique<PortfolioRiskEngine>(loadPortfolioConfig(options_.portfolio_config));
    }

    void recoverFromCheckpoint()
    {
        resetEngine();
        durable_checkpoint_timestamp_ = checkpoint_store_->latestDecisionTimestamp();
        engine_->restoreLastTimestamp(durable_checkpoint_timestamp_);

        LG_INFO(
            "service=portfolio-risk event=portfolio_risk_recovery_completed checkpoint=latest-only last_decision_timestamp={} account_join_buffer=postgres-bounded",
            durable_checkpoint_timestamp_);
    }

    DurableMessageDisposition onAccountSnapshot(const BusMessage& message)
    {
        try {
            AccountSnapshot snapshot = ContractJsonCodec::decodeAccountSnapshot(message.payload);
            if (snapshot.timestamp == 0 || !std::isfinite(snapshot.cash) || snapshot.metadata.message_id.empty())
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

            const Timestamp newestCompleted = newestCompletedBusinessUtcDate(time_handler_);
            if (snapshot.timestamp > newestCompleted) {
                LG_ALERT(
                    "service=portfolio-risk event=account_snapshot_terminated reason=future_date timestamp={} newest_completed_utc={}",
                    snapshot.timestamp,
                    newestCompleted);
                return DurableMessageDisposition::Terminate;
            }
            if (replay_bootstrap_completed_date_.has_value() &&
                snapshot.timestamp < *replay_bootstrap_completed_date_) {
                LG_INFO(
                    "service=portfolio-risk event=pre_bootstrap_account_snapshot_skipped timestamp={} bootstrap_completed_date={} disposition=ack",
                    snapshot.timestamp,
                    *replay_bootstrap_completed_date_);
                return DurableMessageDisposition::Ack;
            }
            if (!replay_bootstrap_completed_date_.has_value() &&
                snapshot.timestamp < newestCompleted) {
                LG_INFO(
                    "service=portfolio-risk event=stale_account_snapshot_skipped timestamp={} newest_completed_utc={} disposition=ack",
                    snapshot.timestamp,
                    newestCompleted);
                return DurableMessageDisposition::Ack;
            }

            // PortfolioRisk's daily join uses the account snapshot captured at the
            // market-data boundary. Reconciliation snapshots remain useful elsewhere,
            // but they must not replace the deterministic close-T risk input.
            const std::string expectedMessageId =
                "account-snapshot:market-data:" + std::to_string(snapshot.timestamp);
            if (snapshot.metadata.message_id != expectedMessageId) {
                LG_DEBUG(
                    "service=portfolio-risk event=non_daily_account_snapshot_skipped timestamp={} message_id={} expected_message_id={} disposition=ack",
                    snapshot.timestamp,
                    snapshot.metadata.message_id,
                    expectedMessageId);
                return DurableMessageDisposition::Ack;
            }

            const auto existing = checkpoint_store_->accountFor(snapshot.timestamp);
            if (existing.has_value() && existing->message_id == snapshot.metadata.message_id) {
                if (existing->payload != message.payload) {
                    LG_ALERT(
                        "service=portfolio-risk event=account_snapshot_checkpoint_conflict timestamp={} message_id={} disposition=terminate",
                        snapshot.timestamp,
                        snapshot.metadata.message_id);
                    return DurableMessageDisposition::Terminate;
                }
                return DurableMessageDisposition::Ack;
            }

            const Timestamp snapshotTimestamp = snapshot.timestamp;
            const double cash = snapshot.cash;
            const std::size_t physicalPositions = snapshot.positions.size();
            const std::size_t strategyPositionSets = snapshot.strategy_positions.size();
            const std::string messageId = snapshot.metadata.message_id;

            checkpoint_store_->saveAccount(snapshotTimestamp, messageId, message.payload);

            LG_INFO(
                "service=portfolio-risk event=account_snapshot_ready timestamp={} cash={} physical_positions={} strategy_position_sets={} message_id={} storage=postgres-bounded",
                snapshotTimestamp,
                cash,
                physicalPositions,
                strategyPositionSets,
                messageId);
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

            const Timestamp target = signals.timestamp;
            const Timestamp newestCompleted = newestCompletedBusinessUtcDate(time_handler_);
            if (target > newestCompleted) {
                LG_ALERT(
                    "service=portfolio-risk event=strategy_intents_terminated reason=future_completed_date timestamp={} newest_completed_utc={}",
                    target,
                    newestCompleted);
                return DurableMessageDisposition::Terminate;
            }

            const auto persistedDecision = checkpoint_store_->decisionFor(target);
            if (persistedDecision.has_value()) {
                if (persistedDecision->signals_payload != message.payload) {
                    LG_ALERT(
                        "service=portfolio-risk event=decision_checkpoint_conflict timestamp={} disposition=terminate",
                        target);
                    return DurableMessageDisposition::Terminate;
                }
                durable_checkpoint_timestamp_ = std::max(durable_checkpoint_timestamp_, target);
                if (engine_->lastTimestamp() < target)
                    engine_->restoreLastTimestamp(target);
                LG_INFO(
                    "service=portfolio-risk event=strategy_intents_checkpoint_duplicate timestamp={} disposition=ack",
                    target);
                return DurableMessageDisposition::Ack;
            }

            // Identity LIVE mode still acts only on the latest fully completed UTC day.
            // Historical replay with an explicit shared simulated reference is event-time
            // driven: a durable intent at/after the immutable bootstrap day remains valid
            // even if processing latency means TimeHandler has advanced to a later day.
            if (replay_bootstrap_completed_date_.has_value() &&
                target < *replay_bootstrap_completed_date_) {
                LG_INFO(
                    "service=portfolio-risk event=pre_bootstrap_strategy_intents_skipped timestamp={} bootstrap_completed_date={} disposition=ack",
                    target,
                    *replay_bootstrap_completed_date_);
                return DurableMessageDisposition::Ack;
            }
            if (!replay_bootstrap_completed_date_.has_value() && target < newestCompleted) {
                LG_WARN(
                    "service=portfolio-risk event=stale_strategy_intents timestamp={} newest_completed_utc={} disposition=ack",
                    target,
                    newestCompleted);
                return DurableMessageDisposition::Ack;
            }

            if (engine_->lastTimestamp() >= target) {
                LG_WARN(
                    "service=portfolio-risk event=volatile_decision_state_ahead_of_checkpoint timestamp={} engine_timestamp={} action=recover",
                    target,
                    engine_->lastTimestamp());
                recoverFromCheckpoint();
            }

            const auto accountRow = checkpoint_store_->accountFor(target);
            if (!accountRow.has_value()) {
                LG_DEBUG(
                    "service=portfolio-risk event=strategy_intents_waiting timestamp={} missing=account_snapshot disposition=retry",
                    target);
                return DurableMessageDisposition::Retry;
            }

            AccountSnapshot account = ContractJsonCodec::decodeAccountSnapshot(accountRow->payload);
            if (account.timestamp != target || account.metadata.message_id != accountRow->message_id)
                throw std::logic_error("Persisted PortfolioRisk LIVE account checkpoint is inconsistent");

            const std::set<Coin> symbols = requiredSymbols(signals, account);
            MarketData marketData;
            if (symbols.empty()) {
                marketData.emplace(target, CoinBarMap{});
            } else {
                marketData = market_reader_.loadMarketDataWindow(
                    target,
                    required_history_days_,
                    symbols);
            }

            LG_INFO(
                "service=portfolio-risk event=market_window_loaded timestamp={} history_days={} symbols={} rows={} database={}",
                target,
                required_history_days_,
                symbols.size(),
                marketRowCount(marketData),
                options_.market_data_db.string());

            DecisionBatch output = engine_->onSignals(signals, marketData, account);
            output.metadata.schema_version = 1;
            output.metadata.message_id = decisionMessageId(target);
            output.metadata.correlation_id = !signals.metadata.correlation_id.empty()
                ? signals.metadata.correlation_id
                : signals.metadata.message_id;
            output.metadata.produced_at = target;

            const std::string encodedDecision = ContractJsonCodec::encode(output);

            // Preserve the established crash contract: deterministic publish first,
            // durable daily checkpoint second, ACK third. If we crash between publish and
            // checkpoint, retry republishes the same logical MessageID.
            bus_.publish(
                TransportSubjects::DECISION_BATCH,
                encodedDecision,
                output.metadata.message_id);

            checkpoint_store_->saveDecision(
                target,
                message.payload,
                accountRow->payload,
                encodedDecision);
            durable_checkpoint_timestamp_ = target;

            std::size_t decisions = 0;
            for (const StrategyDecisionIntent& strategy : output.strategies)
                decisions += strategy.decisions.size();

            LG_INFO(
                "service=portfolio-risk event=decision_published timestamp={} strategies={} decisions={} reference_cash={} message_id={} correlation_id={}",
                output.decision_timestamp,
                output.strategies.size(),
                decisions,
                account.cash,
                output.metadata.message_id,
                output.metadata.correlation_id);
            LG_INFO(
                "service=portfolio-risk event=portfolio_risk_checkpoint_committed timestamp={} checkpoint=decision-plus-account-reference market_history_retained_in_ram=false",
                target);

            // marketData is a local value and is released here when this callback returns.
            return DurableMessageDisposition::Ack;
        }
        catch (const std::logic_error& error) {
            LG_ALERT("service=portfolio-risk event=strategy_intents_rejected disposition=terminate error={}", error.what());
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
        result.max_ack_pending = 32;
        return result;
    }

public:
    explicit PortfolioRiskServiceRuntime(Options options)
        : options_(std::move(options)),
          required_history_days_(requiredMarketHistoryDays(
              options_.portfolio_config,
              options_.market_history_buffer_days)),
          time_config_(TimeHandlerFactory::loadConfigFromEnvironment()),
          time_handler_(TimeHandlerFactory::create(time_config_)),
          replay_bootstrap_completed_date_(configuredReplayBootstrapCompletedUtcDate(time_config_)),
          bus_(options_.nats_url),
          market_reader_(options_.market_data_db)
    {
        bus_.ensureStream(options_.stream, TransportSubjects::tradingRuntimeSubjects());

        checkpoint_store_ = std::make_unique<PortfolioRiskCheckpointStore>(
            options_.postgres,
            checkpointIdentity(options_, required_history_days_));
        recoverFromCheckpoint();

        account_subscription_ = bus_.subscribe(
            consumer("portfolio-risk-account-snapshots", TransportSubjects::ACCOUNT_SNAPSHOT),
            [this](const BusMessage& message) { return onAccountSnapshot(message); });

        strategy_subscription_ = bus_.subscribe(
            consumer("portfolio-risk-strategy-intents", TransportSubjects::STRATEGY_INTENTS),
            [this](const BusMessage& message) { return onStrategyIntents(message); });
    }

    ~PortfolioRiskServiceRuntime()
    {
        bus_.close(strategy_subscription_);
        bus_.close(account_subscription_);
    }

    void run()
    {
        constexpr auto loopPeriod = std::chrono::milliseconds(1000);
        constexpr std::int64_t fetchTimeoutMs = 100;

        LG_INFO(
            "service=portfolio-risk event=service_ready mode=live time_source=time_handler stream={} config={} market_database={} market_history_days={} input_strategy_subject={} input_account_subject={} restart_checkpoint=postgres-latest-only loop_period_ms={} time_speed={} time_identity={}",
            options_.stream,
            options_.portfolio_config,
            options_.market_data_db.string(),
            required_history_days_,
            TransportSubjects::STRATEGY_INTENTS,
            TransportSubjects::ACCOUNT_SNAPSHOT,
            loopPeriod.count(),
            time_config_.speed,
            time_config_.identity());
        std::cout.flush();

        bool waitingForNextUtcDayLogged = false;
        Timestamp loggedCompletedDate = 0;

        while (running.load()) {
            const Timestamp newestCompleted = newestCompletedBusinessUtcDate(time_handler_);

            if (durable_checkpoint_timestamp_ >= newestCompleted) {
                if (!waitingForNextUtcDayLogged || loggedCompletedDate != newestCompleted) {
                    LG_INFO(
                        "service=portfolio-risk event=daily_checkpoint_complete timestamp={} action=wait_until_next_utc_day",
                        durable_checkpoint_timestamp_);
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
            const auto iterationStarted = std::chrono::steady_clock::now();

            // Account state first, Strategy intent second. If the intent arrives first,
            // it stays unACKed (Retry) until the matching account snapshot is available.
            bus_.poll(account_subscription_, 32, fetchTimeoutMs);
            bus_.poll(strategy_subscription_, 16, fetchTimeoutMs);

            const auto elapsed = std::chrono::steady_clock::now() - iterationStarted;
            if (elapsed < loopPeriod)
                interruptibleSleepFor(loopPeriod - elapsed);
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
