#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>

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


class ReplayControllerRuntime {
private:
    const Options options_;
    NatsJetStreamMessageBus bus_;
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
