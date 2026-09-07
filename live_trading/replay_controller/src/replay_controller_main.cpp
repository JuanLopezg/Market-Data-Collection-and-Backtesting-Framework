#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <libpq-fe.h>

#include "clock.h"
#include "clock_control.h"
#include "clock_state.h"
#include "clock_sync_request.h"
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
    std::string simulation_id;
    int barrier_timeout_ms = 30000;
    int poll_timeout_ms = 100;
    SimulationClockMode initial_clock_mode = SimulationClockMode::MaxSpeed;
    double initial_speed_multiplier = 0.0;
    double clock_wall_scale = 1.0;
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


const char* clockModeName(SimulationClockMode mode)
{
    switch (mode) {
    case SimulationClockMode::Realtime:
        return "realtime";
    case SimulationClockMode::Multiplier:
        return "multiplier";
    case SimulationClockMode::MaxSpeed:
        return "max";
    }
    return "unknown";
}


struct ClockSpeedSpec {
    SimulationClockMode mode = SimulationClockMode::MaxSpeed;
    double multiplier = 0.0;
};


ClockSpeedSpec parseClockSpeed(std::string value)
{
    for (char& ch : value)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

    if (value == "max")
        return {SimulationClockMode::MaxSpeed, 0.0};
    if (value == "realtime" || value == "x1" || value == "1")
        return {SimulationClockMode::Realtime, 0.0};

    if (!value.empty() && value.front() == 'x')
        value.erase(value.begin());

    std::size_t consumed = 0;
    const double multiplier = std::stod(value, &consumed);
    if (consumed != value.size() || !std::isfinite(multiplier) || multiplier <= 0.0)
        throw std::invalid_argument("Invalid --clock-speed value");
    if (multiplier == 1.0)
        return {SimulationClockMode::Realtime, 0.0};
    return {SimulationClockMode::Multiplier, multiplier};
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
        else if (arg == "--simulation-id")
            options.simulation_id = requireValue("--simulation-id");
        else if (arg == "--barrier-timeout-ms")
            options.barrier_timeout_ms = std::stoi(requireValue("--barrier-timeout-ms"));
        else if (arg == "--poll-timeout-ms")
            options.poll_timeout_ms = std::stoi(requireValue("--poll-timeout-ms"));
        else if (arg == "--clock-speed") {
            const ClockSpeedSpec speed = parseClockSpeed(requireValue("--clock-speed"));
            options.initial_clock_mode = speed.mode;
            options.initial_speed_multiplier = speed.multiplier;
        }
        else if (arg == "--clock-wall-scale")
            options.clock_wall_scale = std::stod(requireValue("--clock-wall-scale"));
        else if (arg == "--help" || arg == "-h") {
            std::cout
                << "Replay-controller options:\n"
                << "  --nats-url URL\n"
                << "  --stream NAME\n"
                << "  --start-date YYYY-MM-DD   first decision close\n"
                << "  --end-date YYYY-MM-DD     last decision close\n"
                << "  --postgres CONNECTION     durable replay progress checkpoint\n"
                << "  --simulation-id ID        stable logical-clock identity (optional)\n"
                << "  --barrier-timeout-ms N\n"
                << "  --poll-timeout-ms N\n"
                << "  --clock-speed max|x1|xN  initial REPLAY pacing mode\n"
                << "  --clock-wall-scale X     wall-time scale (1.0=true time; validation may compress)\n";
            std::exit(0);
        }
        else
            throw std::invalid_argument("Unknown option: " + arg);
    }

    if (options.stream.empty() || options.start_date.empty() || options.end_date.empty())
        throw std::invalid_argument("Replay stream/start/end options cannot be empty");
    if (options.barrier_timeout_ms <= 0 || options.poll_timeout_ms <= 0)
        throw std::invalid_argument("Replay timeout options must be positive");
    if (!std::isfinite(options.clock_wall_scale) || options.clock_wall_scale <= 0.0)
        throw std::invalid_argument("--clock-wall-scale must be finite and positive");

    const auto start = std::chrono::sys_days{parseDate(options.start_date)};
    const auto end = std::chrono::sys_days{parseDate(options.end_date)};
    if (start > end)
        throw std::invalid_argument("--start-date must be <= --end-date");

    if (options.simulation_id.empty())
        options.simulation_id = "replay:" + options.start_date + ":" + options.end_date;

    return options;
}


std::chrono::system_clock::time_point logicalClockPoint(Timestamp value)
{
    if (value == 0)
        throw std::invalid_argument("Replay logical timestamp cannot be zero");

    const std::chrono::year_month_day date{
        std::chrono::year{static_cast<int>(value / 10000U)},
        std::chrono::month{static_cast<unsigned>((value / 100U) % 100U)},
        std::chrono::day{static_cast<unsigned>(value % 100U)}
    };
    if (!date.ok())
        throw std::invalid_argument("Replay logical timestamp is not a valid YYYYMMDD date");
    return std::chrono::sys_days{date};
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

        exec(
            "CREATE TABLE IF NOT EXISTS replay_controller_clock_state ("
            "state_key TEXT PRIMARY KEY, "
            "simulation_id TEXT NOT NULL, "
            "logical_time BIGINT NOT NULL, "
            "revision NUMERIC(20,0) NOT NULL, "
            "mode INTEGER NOT NULL, "
            "speed_multiplier DOUBLE PRECISION NOT NULL, "
            "paused BOOLEAN NOT NULL, "
            "message_id TEXT NOT NULL"
            ")",
            PGRES_COMMAND_OK
        );

        exec(
            "ALTER TABLE replay_controller_clock_state "
            "ALTER COLUMN revision TYPE NUMERIC(20,0) USING revision::numeric",
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

    std::optional<ClockState> loadClockState() const
    {
        const PgResult result = execParams(
            "SELECT simulation_id, logical_time, revision, mode, speed_multiplier, "
            "paused, message_id FROM replay_controller_clock_state WHERE state_key = $1",
            {STATE_KEY},
            PGRES_TUPLES_OK
        );

        if (PQntuples(result.get()) == 0)
            return std::nullopt;
        if (PQntuples(result.get()) != 1)
            throw std::runtime_error("Replay clock checkpoint returned multiple rows");

        ClockState value;
        value.metadata.schema_version = 1;
        value.metadata.message_id = PQgetvalue(result.get(), 0, 6);
        value.metadata.correlation_id = "replay-controller";
        value.simulation_id = PQgetvalue(result.get(), 0, 0);
        value.logical_time = static_cast<Timestamp>(
            std::stoull(PQgetvalue(result.get(), 0, 1))
        );
        value.metadata.produced_at = value.logical_time;
        value.revision = static_cast<std::uint64_t>(
            std::stoull(PQgetvalue(result.get(), 0, 2))
        );
        value.mode = static_cast<SimulationClockMode>(
            std::stoi(PQgetvalue(result.get(), 0, 3))
        );
        value.speed_multiplier = std::stod(PQgetvalue(result.get(), 0, 4));
        value.paused = std::string(PQgetvalue(result.get(), 0, 5)) == "t";
        return value;
    }

    void recordClockState(const ClockState& value) const
    {
        const PgResult existing = execParams(
            "SELECT simulation_id, logical_time, revision, mode, speed_multiplier, paused, "
            "message_id FROM replay_controller_clock_state WHERE state_key = $1",
            {STATE_KEY},
            PGRES_TUPLES_OK
        );

        if (PQntuples(existing.get()) > 1)
            throw std::runtime_error("Replay clock checkpoint returned multiple rows");

        if (PQntuples(existing.get()) == 1) {
            const std::string storedSimulation = PQgetvalue(existing.get(), 0, 0);
            const Timestamp storedTime = static_cast<Timestamp>(
                std::stoull(PQgetvalue(existing.get(), 0, 1))
            );
            const std::uint64_t storedRevision = static_cast<std::uint64_t>(
                std::stoull(PQgetvalue(existing.get(), 0, 2))
            );

            if (storedSimulation != value.simulation_id)
                throw std::runtime_error("Replay clock simulation_id conflicts with durable state");
            if (value.revision < storedRevision || value.logical_time < storedTime)
                throw std::runtime_error("Replay clock cannot move durable state backwards");

            if (value.revision == storedRevision) {
                const int storedMode = std::stoi(PQgetvalue(existing.get(), 0, 3));
                const double storedSpeed = std::stod(PQgetvalue(existing.get(), 0, 4));
                const bool storedPaused = std::string(PQgetvalue(existing.get(), 0, 5)) == "t";
                const std::string storedMessageId = PQgetvalue(existing.get(), 0, 6);
                if (value.logical_time != storedTime ||
                    static_cast<int>(value.mode) != storedMode ||
                    value.speed_multiplier != storedSpeed ||
                    value.paused != storedPaused ||
                    value.metadata.message_id != storedMessageId)
                    throw std::runtime_error("Replay clock same revision conflicts with durable state");
                return;
            }
        }

        execParams(
            "INSERT INTO replay_controller_clock_state("
            "state_key, simulation_id, logical_time, revision, mode, speed_multiplier, paused, "
            "message_id) VALUES($1, $2, $3, $4, $5, $6, $7, $8) "
            "ON CONFLICT(state_key) DO UPDATE SET "
            "simulation_id = EXCLUDED.simulation_id, logical_time = EXCLUDED.logical_time, "
            "revision = EXCLUDED.revision, mode = EXCLUDED.mode, "
            "speed_multiplier = EXCLUDED.speed_multiplier, paused = EXCLUDED.paused, "
            "message_id = EXCLUDED.message_id",
            {
                STATE_KEY,
                value.simulation_id,
                std::to_string(value.logical_time),
                std::to_string(value.revision),
                std::to_string(static_cast<int>(value.mode)),
                std::to_string(value.speed_multiplier),
                value.paused ? "true" : "false",
                value.metadata.message_id
            },
            PGRES_COMMAND_OK
        );
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
    DurableMessageBus::SubscriptionID clock_sync_subscription_ = 0;
    DurableMessageBus::SubscriptionID clock_control_subscription_ = 0;

    SimulatedClock authority_clock_;
    std::map<Timestamp, std::string> decisions_ready_;
    std::map<Timestamp, Timestamp> executions_complete_;
    std::optional<ClockState> clock_state_;
    std::chrono::steady_clock::time_point next_clock_refresh_at_{};

    static constexpr auto CLOCK_REFRESH_INTERVAL = std::chrono::seconds(1);

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

    static std::string clockMessageId(const std::string& simulationId, std::uint64_t revision)
    {
        return "replay-clock:" + simulationId + ":" + std::to_string(revision);
    }

    void publishClockState(const ClockState& state)
    {
        bus_.publish(
            TransportSubjects::CLOCK_STATE,
            ContractJsonCodec::encode(state),
            state.metadata.message_id
        );
        LG_INFO(
            "service=replay-controller event=clock_state_published simulation_id={} logical_time={} revision={} mode={} speed_multiplier={} paused={} message_id={}",
            state.simulation_id,
            state.logical_time,
            state.revision,
            clockModeName(state.mode),
            state.speed_multiplier,
            state.paused ? "true" : "false",
            state.metadata.message_id
        );
    }

    void scheduleClockRefresh()
    {
        next_clock_refresh_at_ =
            std::chrono::steady_clock::now() + CLOCK_REFRESH_INTERVAL;
    }

    void maybeRefreshClockState()
    {
        if (!clock_state_)
            return;

        const auto now = std::chrono::steady_clock::now();
        if (now < next_clock_refresh_at_)
            return;

        // This is a transport/control-plane refresh only: logical_time and revision do
        // not change and PostgreSQL is not rewritten.  A fresh message id makes the
        // same semantic state observable to a hard-restarted durable follower even if
        // its explicit sync request is delayed by process/bootstrap work.
        ClockState snapshot = *clock_state_;
        const auto ticks = now.time_since_epoch().count();
        snapshot.metadata.message_id =
            "replay-clock-refresh:" + options_.simulation_id + ":" +
            std::to_string(snapshot.revision) + ":" +
            std::to_string(static_cast<std::uint64_t>(ticks));
        snapshot.metadata.correlation_id = "replay-controller-refresh";
        publishClockState(snapshot);
        next_clock_refresh_at_ = now + CLOCK_REFRESH_INTERVAL;

        LG_INFO(
            "service=replay-controller event=clock_state_refresh simulation_id={} logical_time={} revision={} message_id={}",
            snapshot.simulation_id,
            snapshot.logical_time,
            snapshot.revision,
            snapshot.metadata.message_id
        );
    }

    DurableMessageDisposition onClockSyncRequest(const BusMessage& message)
    {
        try {
            const ClockSyncRequest request =
                ContractJsonCodec::decodeClockSyncRequest(message.payload);

            if (!request.simulation_id.empty() && request.simulation_id != options_.simulation_id) {
                LG_WARN(
                    "service=replay-controller event=clock_sync_request_rejected disposition=terminate requester={} requested_simulation_id={} active_simulation_id={}",
                    request.requester_id,
                    request.simulation_id,
                    options_.simulation_id
                );
                return DurableMessageDisposition::Terminate;
            }

            // On a brand-new replay the first normal ClockState is published immediately before
            // CLOSE(T), so there is nothing to re-publish until advanceClock() runs. ACKing this
            // early request is safe; the follower's CLOCK_STATE durable will receive that first tick.
            if (!clock_state_) {
                LG_DEBUG(
                    "service=replay-controller event=clock_sync_request_deferred requester={} reason=clock_not_initialized",
                    request.requester_id
                );
                return DurableMessageDisposition::Ack;
            }

            ClockState snapshot = *clock_state_;
            snapshot.metadata.message_id =
                "replay-clock-sync:" + options_.simulation_id + ":" +
                std::to_string(snapshot.revision) + ":" + request.metadata.message_id;
            snapshot.metadata.correlation_id = request.metadata.message_id;
            publishClockState(snapshot);
            scheduleClockRefresh();

            LG_INFO(
                "service=replay-controller event=clock_sync_response requester={} simulation_id={} logical_time={} revision={} requester_known_revision={} correlation_id={}",
                request.requester_id,
                snapshot.simulation_id,
                snapshot.logical_time,
                snapshot.revision,
                request.known_revision,
                request.metadata.message_id
            );
            return DurableMessageDisposition::Ack;
        }
        catch (const std::invalid_argument& error) {
            LG_WARN(
                "service=replay-controller event=clock_sync_request_invalid disposition=terminate error={}",
                error.what()
            );
            return DurableMessageDisposition::Terminate;
        }
        catch (const std::exception& error) {
            LG_ERROR(
                "service=replay-controller event=clock_sync_request_failed disposition=retry error={}",
                error.what()
            );
            return DurableMessageDisposition::Retry;
        }
    }

    static std::string controlStateMessageId(
        const std::string& simulationId,
        std::uint64_t revision,
        const std::string& commandMessageId
    )
    {
        return "replay-clock-control:" + simulationId + ":" +
            std::to_string(revision) + ":" + commandMessageId;
    }

    static bool messageIdEndsWith(const std::string& value, const std::string& suffix)
    {
        return value.size() >= suffix.size() &&
            value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    DurableMessageDisposition onClockControl(const BusMessage& message)
    {
        try {
            const ClockControl command = ContractJsonCodec::decodeClockControl(message.payload);

            if (command.simulation_id != options_.simulation_id) {
                LG_WARN(
                    "service=replay-controller event=clock_control_rejected reason=simulation_id_mismatch disposition=terminate command_simulation_id={} active_simulation_id={} message_id={}",
                    command.simulation_id,
                    options_.simulation_id,
                    command.metadata.message_id
                );
                return DurableMessageDisposition::Terminate;
            }

            if (!clock_state_) {
                LG_DEBUG(
                    "service=replay-controller event=clock_control_deferred reason=clock_not_initialized disposition=retry action={} message_id={}",
                    static_cast<int>(command.action),
                    command.metadata.message_id
                );
                return DurableMessageDisposition::Retry;
            }

            // If a crash happened after the durable clock COMMIT but before this command was ACKed,
            // the recovered ClockState message id still carries the command id.  Treat that transport
            // redelivery as idempotent rather than creating a second semantic revision.
            const std::string appliedSuffix = ":" + command.metadata.message_id;
            if (messageIdEndsWith(clock_state_->metadata.message_id, appliedSuffix)) {
                LG_INFO(
                    "service=replay-controller event=clock_control_duplicate_acked message_id={} revision={}",
                    command.metadata.message_id,
                    clock_state_->revision
                );
                return DurableMessageDisposition::Ack;
            }

            if (command.expected_revision != 0 &&
                command.expected_revision != clock_state_->revision) {
                LG_WARN(
                    "service=replay-controller event=clock_control_rejected reason=revision_conflict disposition=terminate expected_revision={} active_revision={} message_id={}",
                    command.expected_revision,
                    clock_state_->revision,
                    command.metadata.message_id
                );
                return DurableMessageDisposition::Terminate;
            }

            ClockState next = *clock_state_;
            bool changed = false;
            const char* actionName = "unknown";

            switch (command.action) {
            case ClockControlAction::Pause:
                actionName = "pause";
                if (!next.paused) {
                    next.paused = true;
                    changed = true;
                }
                break;
            case ClockControlAction::Resume:
                actionName = "resume";
                if (next.paused) {
                    next.paused = false;
                    changed = true;
                }
                break;
            case ClockControlAction::SetSpeed:
                actionName = "set_speed";
                if (next.mode != command.mode ||
                    next.speed_multiplier != command.speed_multiplier) {
                    next.mode = command.mode;
                    next.speed_multiplier = command.speed_multiplier;
                    changed = true;
                }
                break;
            }

            if (!changed) {
                LG_INFO(
                    "service=replay-controller event=clock_control_noop action={} simulation_id={} logical_time={} revision={} mode={} speed_multiplier={} paused={} message_id={}",
                    actionName,
                    next.simulation_id,
                    next.logical_time,
                    next.revision,
                    clockModeName(next.mode),
                    next.speed_multiplier,
                    next.paused ? "true" : "false",
                    command.metadata.message_id
                );
                return DurableMessageDisposition::Ack;
            }

            ++next.revision;
            next.metadata.schema_version = 1;
            next.metadata.correlation_id = command.metadata.message_id;
            next.metadata.produced_at = next.logical_time;
            next.metadata.message_id = controlStateMessageId(
                next.simulation_id,
                next.revision,
                command.metadata.message_id
            );

            if (checkpoint_store_)
                checkpoint_store_->recordClockState(next); // COMMIT before publish/ACK

            authority_clock_.synchronize(logicalClockPoint(next.logical_time), next.revision);
            clock_state_ = next;
            publishClockState(*clock_state_);
            scheduleClockRefresh();

            LG_INFO(
                "service=replay-controller event=clock_control_applied action={} simulation_id={} logical_time={} revision={} mode={} speed_multiplier={} paused={} command_message_id={}",
                actionName,
                clock_state_->simulation_id,
                clock_state_->logical_time,
                clock_state_->revision,
                clockModeName(clock_state_->mode),
                clock_state_->speed_multiplier,
                clock_state_->paused ? "true" : "false",
                command.metadata.message_id
            );
            std::cout.flush();
            return DurableMessageDisposition::Ack;
        }
        catch (const std::invalid_argument& error) {
            LG_WARN(
                "service=replay-controller event=clock_control_invalid disposition=terminate error={}",
                error.what()
            );
            return DurableMessageDisposition::Terminate;
        }
        catch (const std::exception& error) {
            LG_ERROR(
                "service=replay-controller event=clock_control_failed disposition=retry error={}",
                error.what()
            );
            return DurableMessageDisposition::Retry;
        }
    }

    void pollClockControlPlane(int timeoutMs)
    {
        const int pollMs = timeoutMs > 0 ? timeoutMs : 1;
        maybeRefreshClockState();
        bus_.poll(clock_control_subscription_, 16, pollMs);
        bus_.poll(clock_sync_subscription_, 16, pollMs);
    }

    void waitForClockPermissionAndPacing(Timestamp logicalTime)
    {
        if (!clock_state_)
            return; // the first logical timestamp establishes the replay epoch immediately

        const Timestamp fromTime = clock_state_->logical_time;
        if (logicalTime < fromTime)
            throw std::runtime_error("Replay logical clock attempted to move backwards");

        double remainingSimulatedSeconds = 0.0;
        if (logicalTime > fromTime) {
            remainingSimulatedSeconds = std::chrono::duration<double>(
                logicalClockPoint(logicalTime) - logicalClockPoint(fromTime)
            ).count();
        }

        auto lastWall = std::chrono::steady_clock::now();
        const int controlPollMs = options_.poll_timeout_ms < 25
            ? options_.poll_timeout_ms
            : 25;

        while (running.load()) {
            pollClockControlPlane(controlPollMs);
            const auto nowWall = std::chrono::steady_clock::now();

            if (!clock_state_)
                throw std::runtime_error("Replay clock state disappeared during pacing");
            if (clock_state_->logical_time != fromTime)
                throw std::runtime_error("Replay clock changed logical time outside authority advance");

            if (clock_state_->paused) {
                lastWall = nowWall;
                continue;
            }

            if (logicalTime == fromTime || clock_state_->mode == SimulationClockMode::MaxSpeed)
                return;

            const double speed = clock_state_->mode == SimulationClockMode::Realtime
                ? 1.0
                : clock_state_->speed_multiplier;
            if (!std::isfinite(speed) || speed <= 0.0)
                throw std::runtime_error("Replay pacing has invalid active speed");

            const double wallSeconds = std::chrono::duration<double>(nowWall - lastWall).count();
            lastWall = nowWall;
            remainingSimulatedSeconds -= wallSeconds * speed / options_.clock_wall_scale;
            if (remainingSimulatedSeconds <= 0.0)
                return;
        }

        throw std::runtime_error("Replay interrupted while waiting for logical clock pacing");
    }

    void advanceClock(Timestamp logicalTime)
    {
        if (logicalTime == 0)
            throw std::invalid_argument("Replay logical time cannot be zero");

        if (clock_state_ && logicalTime < clock_state_->logical_time)
            throw std::runtime_error("Replay logical clock attempted to move backwards");

        waitForClockPermissionAndPacing(logicalTime);

        if (clock_state_ && logicalTime == clock_state_->logical_time) {
            publishClockState(*clock_state_);
            scheduleClockRefresh();
            return;
        }

        ClockState next;
        next.metadata.schema_version = 1;
        next.metadata.correlation_id = "replay-controller";
        next.simulation_id = options_.simulation_id;
        next.logical_time = logicalTime;
        next.revision = clock_state_ ? clock_state_->revision + 1 : 1;
        next.mode = clock_state_ ? clock_state_->mode : options_.initial_clock_mode;
        next.speed_multiplier = clock_state_
            ? clock_state_->speed_multiplier
            : options_.initial_speed_multiplier;
        next.paused = clock_state_ ? clock_state_->paused : false;
        next.metadata.produced_at = logicalTime;
        next.metadata.message_id = clockMessageId(next.simulation_id, next.revision);

        if (checkpoint_store_)
            checkpoint_store_->recordClockState(next); // COMMIT before publish

        authority_clock_.synchronize(logicalClockPoint(next.logical_time), next.revision);
        clock_state_ = next;
        publishClockState(*clock_state_);
        scheduleClockRefresh();
    }

    void recoverClockThrough(Timestamp logicalTime)
    {
        if (!clock_state_ || clock_state_->logical_time < logicalTime)
            advanceClock(logicalTime);
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
        const auto timeout = std::chrono::milliseconds(options_.barrier_timeout_ms);
        auto activeWait = std::chrono::steady_clock::duration::zero();
        auto lastWall = std::chrono::steady_clock::now();

        while (running.load() && !ready()) {
            maybeRefreshClockState();
            bus_.poll(clock_control_subscription_, 16, options_.poll_timeout_ms);
            bus_.poll(clock_sync_subscription_, 16, options_.poll_timeout_ms);
            bus_.poll(decision_subscription_, 16, options_.poll_timeout_ms);
            bus_.poll(execution_subscription_, 16, options_.poll_timeout_ms);

            const auto nowWall = std::chrono::steady_clock::now();
            const bool paused = clock_state_ && clock_state_->paused;
            if (!paused)
                activeWait += nowWall - lastWall;
            lastWall = nowWall;

            // Operator pause is a logical-clock state, not a correctness timeout.  The
            // technical barrier deadline therefore stops accumulating while paused.
            if (activeWait >= timeout) {
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
            clock_state_ = checkpoint_store_->loadClockState();
            if (clock_state_ && clock_state_->simulation_id != options_.simulation_id)
                throw std::runtime_error("Recovered logical clock simulation_id does not match --simulation-id");
            if (clock_state_)
                authority_clock_.synchronize(
                    logicalClockPoint(clock_state_->logical_time),
                    clock_state_->revision
                );
            LG_INFO(
                "service=replay-controller event=replay_recovery_completed recovered_decisions={} recovered_executions={} clock_recovered={} clock_time={} clock_revision={} clock_mode={} speed_multiplier={} paused={} simulation_id={} range_start={} range_end={}",
                decisions_ready_.size(),
                executions_complete_.size(),
                clock_state_ ? "true" : "false",
                clock_state_ ? clock_state_->logical_time : 0,
                clock_state_ ? clock_state_->revision : 0,
                clock_state_ ? clockModeName(clock_state_->mode) : clockModeName(options_.initial_clock_mode),
                clock_state_ ? clock_state_->speed_multiplier : options_.initial_speed_multiplier,
                clock_state_ && clock_state_->paused ? "true" : "false",
                options_.simulation_id,
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
        clock_sync_subscription_ = bus_.subscribe(
            consumer(
                "replay-controller-clock-sync",
                TransportSubjects::CLOCK_SYNC_REQUEST
            ),
            [this](const BusMessage& message) { return onClockSyncRequest(message); }
        );
        clock_control_subscription_ = bus_.subscribe(
            consumer(
                "replay-controller-clock-control",
                TransportSubjects::CLOCK_CONTROL
            ),
            [this](const BusMessage& message) { return onClockControl(message); }
        );

        // A crash may occur after PostgreSQL COMMIT but before NATS publish. Re-emitting the
        // deterministic message_id is safe under JetStream deduplication and lets late/restarted
        // REPLAY consumers recover the authoritative state.
        if (clock_state_) {
            publishClockState(*clock_state_);
            scheduleClockRefresh();
        }
    }

    ~ReplayControllerRuntime()
    {
        bus_.close(clock_control_subscription_);
        bus_.close(clock_sync_subscription_);
        bus_.close(execution_subscription_);
        bus_.close(decision_subscription_);
    }

    void run()
    {
        const auto start = std::chrono::sys_days{parseDate(options_.start_date)};
        const auto end = std::chrono::sys_days{parseDate(options_.end_date)};

        std::size_t completed = 0;
        const SimulationClockMode readyMode = clock_state_
            ? clock_state_->mode
            : options_.initial_clock_mode;
        const double readySpeed = clock_state_
            ? clock_state_->speed_multiplier
            : options_.initial_speed_multiplier;
        const bool readyPaused = clock_state_ && clock_state_->paused;
        LG_INFO(
            "service=replay-controller event=service_ready range_start={} range_end={} stream={} simulation_id={} clock_mode={} speed_multiplier={} paused={} clock_wall_scale={} barrier_timeout_ms={} poll_timeout_ms={}",
            options_.start_date,
            options_.end_date,
            options_.stream,
            options_.simulation_id,
            clockModeName(readyMode),
            readySpeed,
            readyPaused ? "true" : "false",
            options_.clock_wall_scale,
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
                recoverClockThrough(executionTimestamp);
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
                advanceClock(decisionTimestamp);
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

            advanceClock(executionTimestamp);
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
