#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <libpq-fe.h>

#include "contract_json_codec.h"
#include "decision_batch.h"
#include "execution_cycle_complete.h"
#include "market_data_release.h"
#include "nats_jetstream_message_bus.h"
#include "service_logging.h"
#include "transport_subjects.h"


namespace {

std::atomic<bool> running{true};

void stopHandler(int)
{
    running.store(false);
}


struct Options {
    std::string nats_url = "nats://127.0.0.1:4222";
    std::string stream = "ALGOTRADING_RUNTIME";
    std::string start_date;
    std::string end_date;
    std::string postgres;
    int barrier_timeout_ms = 30000;
    int poll_timeout_ms = 100;
};


std::chrono::year_month_day parseDate(const std::string& value)
{
    if (value.size() != 10 || value[4] != '-' || value[7] != '-')
        throw std::invalid_argument("Date must use YYYY-MM-DD");

    const std::chrono::year_month_day result{
        std::chrono::year{std::stoi(value.substr(0, 4))},
        std::chrono::month{static_cast<unsigned>(std::stoi(value.substr(5, 2)))},
        std::chrono::day{static_cast<unsigned>(std::stoi(value.substr(8, 2)))}
    };

    if (!result.ok())
        throw std::invalid_argument("Invalid calendar date: " + value);
    return result;
}


std::string formatDate(std::chrono::year_month_day date)
{
    char buffer[11];
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%04d-%02u-%02u",
        static_cast<int>(date.year()),
        static_cast<unsigned>(date.month()),
        static_cast<unsigned>(date.day())
    );
    return buffer;
}


Timestamp toTimestamp(std::chrono::year_month_day date)
{
    const unsigned long value =
        static_cast<unsigned long>(static_cast<int>(date.year())) * 10000UL +
        static_cast<unsigned long>(static_cast<unsigned>(date.month())) * 100UL +
        static_cast<unsigned long>(static_cast<unsigned>(date.day()));

    return static_cast<Timestamp>(value);
}


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
            options.stream = requireValue("--stream");
        else if (arg == "--start-date")
            options.start_date = requireValue("--start-date");
        else if (arg == "--end-date")
            options.end_date = requireValue("--end-date");
        else if (arg == "--postgres")
            options.postgres = requireValue("--postgres");
        else if (arg == "--barrier-timeout-ms")
            options.barrier_timeout_ms = std::stoi(requireValue("--barrier-timeout-ms"));
        else if (arg == "--poll-timeout-ms")
            options.poll_timeout_ms = std::stoi(requireValue("--poll-timeout-ms"));
        else if (arg == "--help" || arg == "-h") {
            std::cout
                << "Replay-controller options:\n"
                << "  --nats-url URL\n"
                << "  --stream NAME\n"
                << "  --start-date YYYY-MM-DD   first decision close\n"
                << "  --end-date YYYY-MM-DD     last decision close\n"
                << "  --postgres CONNECTION     durable replay progress checkpoint\n"
                << "  --barrier-timeout-ms N\n"
                << "  --poll-timeout-ms N\n";
            std::exit(0);
        }
        else
            throw std::invalid_argument("Unknown option: " + arg);
    }

    if (options.stream.empty() || options.start_date.empty() || options.end_date.empty())
        throw std::invalid_argument("Replay stream/start/end options cannot be empty");
    if (options.barrier_timeout_ms <= 0 || options.poll_timeout_ms <= 0)
        throw std::invalid_argument("Replay timeout options must be positive");

    const auto start = std::chrono::sys_days{parseDate(options.start_date)};
    const auto end = std::chrono::sys_days{parseDate(options.end_date)};
    if (start > end)
        throw std::invalid_argument("--start-date must be <= --end-date");

    return options;
}


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


class ReplayCheckpointStore {
private:
    PGconn* connection_ = nullptr;
    static constexpr const char* STATE_KEY = "replay-controller";

    void requireConnection() const
    {
        if (connection_ == nullptr || PQstatus(connection_) != CONNECTION_OK)
            throw std::runtime_error("Replay checkpoint PostgreSQL connection is not ready");
    }

    PgResult exec(const std::string& sql, ExecStatusType expected) const
    {
        requireConnection();
        PgResult result(PQexec(connection_, sql.c_str()));
        if (result.get() == nullptr || PQresultStatus(result.get()) != expected) {
            const std::string error = connection_ ? PQerrorMessage(connection_) : "unknown PostgreSQL error";
            throw std::runtime_error("Replay checkpoint PostgreSQL query failed: " + error);
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
            throw std::runtime_error("Replay checkpoint PostgreSQL parameterized query failed: " + error);
        }
        return result;
    }

    void ensureSchema(Timestamp rangeStart, Timestamp rangeEnd)
    {
        exec(
            "CREATE TABLE IF NOT EXISTS replay_controller_metadata ("
            "state_key TEXT PRIMARY KEY, "
            "range_start BIGINT NOT NULL, "
            "range_end BIGINT NOT NULL"
            ")",
            PGRES_COMMAND_OK
        );

        exec(
            "CREATE TABLE IF NOT EXISTS replay_controller_decision_checkpoint ("
            "state_key TEXT NOT NULL, "
            "decision_timestamp BIGINT NOT NULL, "
            "message_id TEXT NOT NULL, "
            "PRIMARY KEY(state_key, decision_timestamp)"
            ")",
            PGRES_COMMAND_OK
        );

        exec(
            "CREATE TABLE IF NOT EXISTS replay_controller_execution_checkpoint ("
            "state_key TEXT NOT NULL, "
            "decision_timestamp BIGINT NOT NULL, "
            "execution_timestamp BIGINT NOT NULL, "
            "state_revision NUMERIC(20,0) NOT NULL, "
            "message_id TEXT NOT NULL, "
            "PRIMARY KEY(state_key, decision_timestamp)"
            ")",
            PGRES_COMMAND_OK
        );

        // state_revision is a std::uint64_t and may exceed PostgreSQL BIGINT's
        // signed 64-bit range. 33B initially created this column as BIGINT, so
        // migrate existing validation databases in-place as well as using the
        // correct type for fresh databases. NUMERIC(20,0) losslessly covers the
        // complete uint64_t range [0, 18446744073709551615].
        exec(
            "ALTER TABLE replay_controller_execution_checkpoint "
            "ALTER COLUMN state_revision TYPE NUMERIC(20,0) "
            "USING state_revision::numeric",
            PGRES_COMMAND_OK
        );

        execParams(
            "INSERT INTO replay_controller_metadata(state_key, range_start, range_end) "
            "VALUES($1, $2, $3) ON CONFLICT(state_key) DO NOTHING",
            {STATE_KEY, std::to_string(rangeStart), std::to_string(rangeEnd)},
            PGRES_COMMAND_OK
        );

        const PgResult metadata = execParams(
            "SELECT range_start, range_end FROM replay_controller_metadata WHERE state_key = $1",
            {STATE_KEY},
            PGRES_TUPLES_OK
        );

        if (PQntuples(metadata.get()) != 1)
            throw std::runtime_error("Replay checkpoint metadata row missing");

        const Timestamp storedStart = static_cast<Timestamp>(std::stoull(PQgetvalue(metadata.get(), 0, 0)));
        const Timestamp storedEnd = static_cast<Timestamp>(std::stoull(PQgetvalue(metadata.get(), 0, 1)));
        if (storedStart != rangeStart || storedEnd != rangeEnd)
            throw std::runtime_error("Replay checkpoint range does not match configured replay range");
    }

public:
    ReplayCheckpointStore(
        const std::string& connectionString,
        Timestamp rangeStart,
        Timestamp rangeEnd
    )
    {
        connection_ = PQconnectdb(connectionString.c_str());
        if (connection_ == nullptr || PQstatus(connection_) != CONNECTION_OK) {
            const std::string error = connection_ ? PQerrorMessage(connection_) : "cannot allocate PGconn";
            if (connection_) {
                PQfinish(connection_);
                connection_ = nullptr;
            }
            throw std::runtime_error("Replay checkpoint PostgreSQL connect failed: " + error);
        }

        ensureSchema(rangeStart, rangeEnd);
    }

    ~ReplayCheckpointStore()
    {
        if (connection_)
            PQfinish(connection_);
    }

    ReplayCheckpointStore(const ReplayCheckpointStore&) = delete;
    ReplayCheckpointStore& operator=(const ReplayCheckpointStore&) = delete;

    std::map<Timestamp, std::string> loadDecisions() const
    {
        const PgResult result = execParams(
            "SELECT decision_timestamp, message_id "
            "FROM replay_controller_decision_checkpoint "
            "WHERE state_key = $1 ORDER BY decision_timestamp",
            {STATE_KEY},
            PGRES_TUPLES_OK
        );

        std::map<Timestamp, std::string> values;
        for (int row = 0; row < PQntuples(result.get()); ++row) {
            values.emplace(
                static_cast<Timestamp>(std::stoull(PQgetvalue(result.get(), row, 0))),
                PQgetvalue(result.get(), row, 1)
            );
        }
        return values;
    }

    std::map<Timestamp, Timestamp> loadExecutions() const
    {
        const PgResult result = execParams(
            "SELECT decision_timestamp, execution_timestamp "
            "FROM replay_controller_execution_checkpoint "
            "WHERE state_key = $1 ORDER BY decision_timestamp",
            {STATE_KEY},
            PGRES_TUPLES_OK
        );

        std::map<Timestamp, Timestamp> values;
        for (int row = 0; row < PQntuples(result.get()); ++row) {
            values.emplace(
                static_cast<Timestamp>(std::stoull(PQgetvalue(result.get(), row, 0))),
                static_cast<Timestamp>(std::stoull(PQgetvalue(result.get(), row, 1)))
            );
        }
        return values;
    }

    void recordDecision(const DecisionBatch& value) const
    {
        execParams(
            "INSERT INTO replay_controller_decision_checkpoint("
            "state_key, decision_timestamp, message_id) VALUES($1, $2, $3) "
            "ON CONFLICT(state_key, decision_timestamp) DO NOTHING",
            {STATE_KEY, std::to_string(value.decision_timestamp), value.metadata.message_id},
            PGRES_COMMAND_OK
        );

        const PgResult existing = execParams(
            "SELECT message_id FROM replay_controller_decision_checkpoint "
            "WHERE state_key = $1 AND decision_timestamp = $2",
            {STATE_KEY, std::to_string(value.decision_timestamp)},
            PGRES_TUPLES_OK
        );

        if (PQntuples(existing.get()) != 1 ||
            value.metadata.message_id != PQgetvalue(existing.get(), 0, 0))
            throw std::runtime_error("Conflicting replay decision checkpoint");
    }

    void recordExecution(const ExecutionCycleComplete& value) const
    {
        execParams(
            "INSERT INTO replay_controller_execution_checkpoint("
            "state_key, decision_timestamp, execution_timestamp, state_revision, message_id) "
            "VALUES($1, $2, $3, $4, $5) "
            "ON CONFLICT(state_key, decision_timestamp) DO NOTHING",
            {
                STATE_KEY,
                std::to_string(value.decision_timestamp),
                std::to_string(value.execution_timestamp),
                std::to_string(value.state_revision),
                value.metadata.message_id
            },
            PGRES_COMMAND_OK
        );

        const PgResult existing = execParams(
            "SELECT execution_timestamp, state_revision, message_id "
            "FROM replay_controller_execution_checkpoint "
            "WHERE state_key = $1 AND decision_timestamp = $2",
            {STATE_KEY, std::to_string(value.decision_timestamp)},
            PGRES_TUPLES_OK
        );

        if (PQntuples(existing.get()) != 1 ||
            static_cast<Timestamp>(std::stoull(PQgetvalue(existing.get(), 0, 0))) != value.execution_timestamp ||
            static_cast<std::uint64_t>(std::stoull(PQgetvalue(existing.get(), 0, 1))) != value.state_revision ||
            value.metadata.message_id != PQgetvalue(existing.get(), 0, 2))
            throw std::runtime_error("Conflicting replay execution checkpoint");
    }
};


class ReplayControllerRuntime {
private:
    const Options options_;
    NatsJetStreamMessageBus bus_;
    std::unique_ptr<ReplayCheckpointStore> checkpoint_store_;
    DurableMessageBus::SubscriptionID decision_subscription_ = 0;
    DurableMessageBus::SubscriptionID execution_subscription_ = 0;

    std::map<Timestamp, std::string> decisions_ready_;
    std::map<Timestamp, Timestamp> executions_complete_;

    DurableConsumerOptions consumer(const std::string& durable, const std::string& subject) const
    {
        DurableConsumerOptions result;
        result.stream = options_.stream;
        result.durable_name = durable;
        result.subject = subject;
        result.ack_wait_ms = 30000;
        result.max_deliver = 20;
        result.max_ack_pending = 64;
        return result;
    }

    DurableMessageDisposition onDecision(const BusMessage& message)
    {
        try {
            const DecisionBatch value = ContractJsonCodec::decodeDecisionBatch(message.payload);
            if (value.metadata.schema_version != 1 || value.metadata.message_id.empty() ||
                value.decision_timestamp == 0)
                return DurableMessageDisposition::Terminate;

            try {
                if (checkpoint_store_)
                    checkpoint_store_->recordDecision(value);
            }
            catch (const std::exception& error) {
                LG_ERROR(
                    "service=replay-controller event=decision_checkpoint_failed disposition=retry decision_timestamp={} error={}",
                    value.decision_timestamp,
                    error.what()
                );
                return DurableMessageDisposition::Retry;
            }

            decisions_ready_[value.decision_timestamp] = value.metadata.message_id;
            LG_DEBUG(
                "service=replay-controller event=decision_barrier_observed decision_timestamp={} message_id={}",
                value.decision_timestamp,
                value.metadata.message_id
            );
            return DurableMessageDisposition::Ack;
        }
        catch (const std::exception& error) {
            LG_ERROR("service=replay-controller event=decision_barrier_decode_failed disposition=terminate error={}", error.what());
            return DurableMessageDisposition::Terminate;
        }
    }

    DurableMessageDisposition onExecutionComplete(const BusMessage& message)
    {
        try {
            const ExecutionCycleComplete value =
                ContractJsonCodec::decodeExecutionCycleComplete(message.payload);
            if (value.metadata.schema_version != 1 || value.metadata.message_id.empty() ||
                value.decision_timestamp == 0 || value.execution_timestamp == 0 ||
                value.execution_timestamp <= value.decision_timestamp ||
                value.state_revision == 0)
                return DurableMessageDisposition::Terminate;

            const auto existing = executions_complete_.find(value.decision_timestamp);
            if (existing != executions_complete_.end() &&
                existing->second != value.execution_timestamp)
                return DurableMessageDisposition::Terminate;

            try {
                if (checkpoint_store_)
                    checkpoint_store_->recordExecution(value);
            }
            catch (const std::exception& error) {
                LG_ERROR(
                    "service=replay-controller event=execution_checkpoint_failed disposition=retry decision_timestamp={} execution_timestamp={} error={}",
                    value.decision_timestamp,
                    value.execution_timestamp,
                    error.what()
                );
                return DurableMessageDisposition::Retry;
            }

            executions_complete_[value.decision_timestamp] = value.execution_timestamp;
            LG_DEBUG(
                "service=replay-controller event=execution_barrier_observed decision_timestamp={} execution_timestamp={} state_revision={} message_id={}",
                value.decision_timestamp,
                value.execution_timestamp,
                value.state_revision,
                value.metadata.message_id
            );
            return DurableMessageDisposition::Ack;
        }
        catch (const std::exception& error) {
            LG_ERROR("service=replay-controller event=execution_barrier_decode_failed disposition=terminate error={}", error.what());
            return DurableMessageDisposition::Terminate;
        }
    }

    void publishRelease(
        MarketDataReleaseKind kind,
        Timestamp timestamp,
        Timestamp decisionTimestamp,
        std::string messageId
    )
    {
        MarketDataReleaseRequest request;
        request.metadata.schema_version = 1;
        request.metadata.message_id = std::move(messageId);
        request.metadata.correlation_id = "replay-controller";
        request.metadata.produced_at = timestamp;
        request.kind = kind;
        request.timestamp = timestamp;
        request.decision_timestamp = decisionTimestamp;

        bus_.publish(
            TransportSubjects::MARKET_DATA_RELEASE,
            ContractJsonCodec::encode(request),
            request.metadata.message_id
        );
        LG_INFO(
            "service=replay-controller event=market_release_published kind={} timestamp={} decision_timestamp={} message_id={}",
            kind == MarketDataReleaseKind::ClosedSlice ? "close" : "execution_open",
            timestamp,
            decisionTimestamp,
            request.metadata.message_id
        );
    }

    template <typename Predicate>
    void waitBarrier(const std::string& name, Predicate&& ready)
    {
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(options_.barrier_timeout_ms);

        while (running.load() && !ready()) {
            bus_.poll(decision_subscription_, 16, options_.poll_timeout_ms);
            bus_.poll(execution_subscription_, 16, options_.poll_timeout_ms);

            if (std::chrono::steady_clock::now() >= deadline) {
                LG_ALERT("service=replay-controller event=barrier_timeout barrier={}", name);
                throw std::runtime_error("Replay barrier timeout: " + name);
            }
        }

        if (!running.load())
            throw std::runtime_error("Replay interrupted while waiting for " + name);
    }

public:
    explicit ReplayControllerRuntime(Options options)
        : options_(std::move(options)),
          bus_(options_.nats_url)
    {
        bus_.ensureStream(options_.stream, TransportSubjects::runtimeSubjects());

        if (!options_.postgres.empty()) {
            const Timestamp rangeStart = toTimestamp(parseDate(options_.start_date));
            const Timestamp rangeEnd = toTimestamp(parseDate(options_.end_date));
            checkpoint_store_ = std::make_unique<ReplayCheckpointStore>(
                options_.postgres,
                rangeStart,
                rangeEnd
            );
            decisions_ready_ = checkpoint_store_->loadDecisions();
            executions_complete_ = checkpoint_store_->loadExecutions();
            LG_INFO(
                "service=replay-controller event=replay_recovery_completed recovered_decisions={} recovered_executions={} range_start={} range_end={}",
                decisions_ready_.size(),
                executions_complete_.size(),
                options_.start_date,
                options_.end_date
            );
        }
        else {
            LG_WARN(
                "service=replay-controller event=restart_checkpoint_disabled reason=postgres_not_configured"
            );
        }

        decision_subscription_ = bus_.subscribe(
            consumer("replay-controller-decisions", TransportSubjects::DECISION_BATCH),
            [this](const BusMessage& message) { return onDecision(message); }
        );
        execution_subscription_ = bus_.subscribe(
            consumer(
                "replay-controller-execution-complete",
                TransportSubjects::EXECUTION_CYCLE_COMPLETE
            ),
            [this](const BusMessage& message) { return onExecutionComplete(message); }
        );
    }

    ~ReplayControllerRuntime()
    {
        bus_.close(execution_subscription_);
        bus_.close(decision_subscription_);
    }

    void run()
    {
        const auto start = std::chrono::sys_days{parseDate(options_.start_date)};
        const auto end = std::chrono::sys_days{parseDate(options_.end_date)};

        std::size_t completed = 0;
        LG_INFO(
            "service=replay-controller event=service_ready range_start={} range_end={} stream={} barrier_timeout_ms={} poll_timeout_ms={}",
            options_.start_date,
            options_.end_date,
            options_.stream,
            options_.barrier_timeout_ms,
            options_.poll_timeout_ms
        );

        for (auto decisionDay = start; decisionDay <= end; decisionDay += std::chrono::days{1}) {
            const auto executionDay = decisionDay + std::chrono::days{1};
            const std::chrono::year_month_day decisionDate{decisionDay};
            const std::chrono::year_month_day executionDate{executionDay};
            const Timestamp decisionTimestamp = toTimestamp(decisionDate);
            const Timestamp executionTimestamp = toTimestamp(executionDate);

            const auto recoveredExecution = executions_complete_.find(decisionTimestamp);
            if (recoveredExecution != executions_complete_.end()) {
                if (recoveredExecution->second != executionTimestamp)
                    throw std::runtime_error("Recovered execution timestamp conflicts with replay calendar");
                ++completed;
                LG_DEBUG(
                    "service=replay-controller event=recovered_cycle_skipped decision_timestamp={} execution_timestamp={} completed_cycles={}",
                    decisionTimestamp,
                    executionTimestamp,
                    completed
                );
                continue;
            }

            if (!decisions_ready_.contains(decisionTimestamp)) {
                publishRelease(
                    MarketDataReleaseKind::ClosedSlice,
                    decisionTimestamp,
                    0,
                    "replay-release-close:" + std::to_string(decisionTimestamp)
                );

                waitBarrier(
                    "decision " + formatDate(decisionDate),
                    [&] { return decisions_ready_.contains(decisionTimestamp); }
                );
                LG_INFO(
                    "[REPLAY] decision-ready close={} service=replay-controller event=decision_ready completed_cycles={}",
                    formatDate(decisionDate),
                    completed
                );
            }
            else {
                LG_INFO(
                    "service=replay-controller event=recovered_decision_barrier decision_timestamp={} completed_cycles={}",
                    decisionTimestamp,
                    completed
                );
            }

            publishRelease(
                MarketDataReleaseKind::ExecutionOpen,
                executionTimestamp,
                decisionTimestamp,
                "replay-release-open:" + std::to_string(decisionTimestamp) + ":" +
                    std::to_string(executionTimestamp)
            );

            waitBarrier(
                "execution " + formatDate(executionDate),
                [&] {
                    const auto it = executions_complete_.find(decisionTimestamp);
                    return it != executions_complete_.end() &&
                        it->second == executionTimestamp;
                }
            );

            ++completed;
            LG_INFO(
                "[REPLAY] execution-complete decision={} open={} service=replay-controller event=execution_complete completed_cycles={}",
                formatDate(decisionDate),
                formatDate(executionDate),
                completed
            );
        }

        bus_.flush();
        LG_INFO(
            "[REPLAY] finished cycles={} service=replay-controller event=replay_finished",
            completed
        );
    }
};

} // namespace


int main(int argc, char** argv)
{
    ServiceLogging::setup("replay-controller");

    try {
        std::signal(SIGINT, stopHandler);
        std::signal(SIGTERM, stopHandler);
        ReplayControllerRuntime runtime(parseOptions(argc, argv));
        runtime.run();
        return 0;
    }
    catch (const std::exception& error) {
        LG_ALERT("service=replay-controller event=fatal error={}", error.what());
        return 1;
    }
}
