#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#include "contract_json_codec.h"
#include "execution_price_snapshot.h"
#include "historical_csv_source.h"
#include "market_data_store.h"
#include "market_data_updated.h"
#include "nats_jetstream_message_bus.h"
#include "service_logging.h"
#include "time_handler_factory.h"
#include "time_utils.h"
#include "transport_subjects.h"

namespace {

constexpr auto EMPTY_DATE = std::chrono::year{2000}/1/1;
std::atomic<bool> running{true};

void stopHandler(int)
{
    running.store(false);
}

struct Options {
    std::string nats_url = "nats://127.0.0.1:4222";
    std::string stream = "ALGOTRADING_HISTORICAL_RUNTIME";
    std::filesystem::path historical_data;
    std::filesystem::path market_data_db = "storage/databases/historical_replay.db";
    std::string source_id = "historical-cmc";
    unsigned int market_top_n = 1000000;
    int poll_timeout_ms = 50;
    bool run_once = false;
    bool init_only = false;
};

void printUsage()
{
    std::cout
        << "Usage: algotrading_historical_market_data_service [options]\n"
        << "  --nats-url URL\n"
        << "  --stream NAME\n"
        << "  --historical-data PATH       CSV: date,symbol,open,high,low,close,volume\n"
        << "  --market-data-db PATH        canonical replay SQLite\n"
        << "  --source-id NAME             default historical-cmc\n"
        << "  --market-top-n N             replay candidate cap; default 1000000 (all source symbols)\n"
        << "  --poll-timeout-ms N          technical polling cadence; default 50\n"
        << "  --run-once                   release all candles currently visible, then exit\n"
        << "  --init-only                  initialize canonical SQLite schema, then exit without NATS/publish\n";
}

Options parseOptions(int argc, char** argv)
{
    Options options;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto requireValue = [&](const char* option) -> std::string {
            if (i + 1 >= argc)
                throw std::invalid_argument(std::string(option) + " requires a value");
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h") {
            printUsage();
            std::exit(0);
        }
        else if (arg == "--nats-url")
            options.nats_url = requireValue("--nats-url");
        else if (arg == "--stream")
            options.stream = requireValue("--stream");
        else if (arg == "--historical-data")
            options.historical_data = requireValue("--historical-data");
        else if (arg == "--market-data-db")
            options.market_data_db = requireValue("--market-data-db");
        else if (arg == "--source-id")
            options.source_id = requireValue("--source-id");
        else if (arg == "--market-top-n")
            options.market_top_n = static_cast<unsigned int>(
                std::stoul(requireValue("--market-top-n")));
        else if (arg == "--poll-timeout-ms")
            options.poll_timeout_ms = std::stoi(requireValue("--poll-timeout-ms"));
        else if (arg == "--run-once")
            options.run_once = true;
        else if (arg == "--init-only")
            options.init_only = true;
        else
            throw std::invalid_argument("Unknown option: " + arg);
    }

    if (options.nats_url.empty() || options.stream.empty() || options.source_id.empty())
        throw std::invalid_argument("Historical market-data string options cannot be empty");
    if (!options.init_only) {
        if (options.historical_data.empty())
            throw std::invalid_argument("--historical-data is required");
        if (!std::filesystem::exists(options.historical_data))
            throw std::invalid_argument("Historical data does not exist: " + options.historical_data.string());
    }
    if (options.market_data_db.empty())
        throw std::invalid_argument("--market-data-db cannot be empty");
    if (options.market_top_n == 0)
        throw std::invalid_argument("--market-top-n must be positive");
    if (options.poll_timeout_ms <= 0 || options.poll_timeout_ms > 60000)
        throw std::invalid_argument("--poll-timeout-ms must be in 1..60000");

    return options;
}

Timestamp newestCompletedBusinessUtcDate(const TimeHandler& timeHandler)
{
    const auto todayUtc = getCurrentUtcDate(timeHandler.getTime());
    const int value = toYYYYMMDD(getPreviousDayDate(todayUtc));
    if (value <= 0)
        throw std::logic_error("Invalid newest completed business UTC date");
    return static_cast<Timestamp>(value);
}

Timestamp currentBusinessUtcDate(const TimeHandler& timeHandler)
{
    const int value = toYYYYMMDD(getCurrentUtcDate(timeHandler.getTime()));
    if (value <= 0)
        throw std::logic_error("Invalid current business UTC date");
    return static_cast<Timestamp>(value);
}

std::chrono::year_month_day fromTimestamp(Timestamp value)
{
    const int year = static_cast<int>(value / 10000U);
    const unsigned month = static_cast<unsigned>((value / 100U) % 100U);
    const unsigned day = static_cast<unsigned>(value % 100U);
    const std::chrono::year_month_day result{
        std::chrono::year{year}, std::chrono::month{month}, std::chrono::day{day}};
    if (!result.ok())
        throw std::invalid_argument("Invalid historical YYYYMMDD timestamp: " + std::to_string(value));
    return result;
}

Timestamp trackedDate(const TrackedMarketData& tracked)
{
    if (tracked.date == EMPTY_DATE || tracked.days_since_top_n.empty())
        return 0;
    const int value = toYYYYMMDD(tracked.date);
    if (value <= 0)
        throw std::logic_error("Invalid canonical tracked-pair checkpoint date");
    return static_cast<Timestamp>(value);
}

TrackedMarketData makeTracked(const HistoricalMarketDay& day)
{
    TrackedMarketData tracked;
    tracked.date = fromTimestamp(day.date);
    for (const auto& [symbol, _] : day.bars.data)
        tracked.days_since_top_n.emplace(symbol, 0);
    return tracked;
}

std::string messageId(Timestamp date)
{
    return "market-data-updated:" + std::to_string(date);
}

std::string executionOpenMessageId(Timestamp date)
{
    return "historical-execution-open:" + std::to_string(date);
}

void publishExecutionOpen(
    NatsJetStreamMessageBus& bus,
    const HistoricalExecutionOpen& day)
{
    if (day.date == 0 || day.prices.empty())
        throw std::invalid_argument("Historical execution open is empty");

    ExecutionPriceSnapshot event;
    event.metadata.schema_version = 1;
    event.metadata.message_id = executionOpenMessageId(day.date);
    event.metadata.correlation_id = event.metadata.message_id;
    event.metadata.produced_at = day.date;
    event.timestamp = day.date;
    event.decision_timestamp = static_cast<Timestamp>(
        previousDay(static_cast<unsigned int>(day.date)));
    event.prices = day.prices;

    bus.publish(
        TransportSubjects::EXECUTION_PRICES,
        ContractJsonCodec::encode(event),
        event.metadata.message_id
    );
    bus.flush();

    LG_INFO(
        "service=historical-market-data event=execution_open_published "
        "execution_timestamp={} decision_timestamp={} prices={} message_id={}",
        event.timestamp,
        event.decision_timestamp,
        event.prices.size(),
        event.metadata.message_id
    );
}

void publishDay(
    NatsJetStreamMessageBus& bus,
    const Options& options,
    const HistoricalMarketDay& day)
{
    MarketDataUpdated event;
    event.metadata.schema_version = 1;
    event.metadata.message_id = messageId(day.date);
    event.metadata.correlation_id = event.metadata.message_id;
    event.metadata.produced_at = day.date;
    event.completed_through = day.date;
    event.source = options.source_id;
    event.timeframe = "1d";
    event.ranked_symbols = static_cast<std::uint64_t>(day.volume_ranking.size());
    event.active_top_n = static_cast<std::uint64_t>(options.market_top_n);
    event.tracked_symbols = static_cast<std::uint64_t>(day.bars.data.size());
    event.maintained_symbols = event.tracked_symbols;
    event.requested_symbols = event.tracked_symbols;
    event.downloaded_rows = static_cast<std::uint64_t>(day.rowCount());

    const std::string payload = ContractJsonCodec::encode(event);
    bus.publish(TransportSubjects::MARKET_DATA_UPDATED, payload, event.metadata.message_id);
    bus.flush();

    LG_INFO(
        "service=historical-market-data event=market_data_updated_published completed_through={} source={} rows={} symbols={} message_id={}",
        day.date,
        event.source,
        event.downloaded_rows,
        event.tracked_symbols,
        event.metadata.message_id
    );
}

void commitAndPublish(
    MarketDataStore& store,
    NatsJetStreamMessageBus& bus,
    const Options& options,
    const HistoricalMarketDay& day)
{
    const TrackedMarketData tracked = makeTracked(day);

    // Critical durability boundary: canonical SQLite is committed before the durable
    // notification. Repeating this function is safe because the DB upserts and NATS
    // message ID are deterministic for the economic date.
    store.commitDailyUpdate(tracked, day.bars, day.volume_ranking, tracked.date);

    LG_INFO(
        "service=historical-market-data event=historical_day_committed date={} rows={} symbols={} database={}",
        day.date,
        day.rowCount(),
        day.bars.data.size(),
        store.path().string()
    );

    publishDay(bus, options, day);
}

} // namespace

int main(int argc, char** argv)
{
    ServiceLogging::setup("historical-market-data");

    try {
        std::signal(SIGINT, stopHandler);
        std::signal(SIGTERM, stopHandler);

        const Options options = parseOptions(argc, argv);

        // Bootstrap path for replay: create/validate canonical SQLite before
        // read-only consumers start. It publishes no market-data event.
        if (options.init_only) {
            MarketDataStore store(options.market_data_db);
            LG_INFO(
                "service=historical-market-data event=market_data_db_initialized database={} mode=init-only",
                store.path().string()
            );
            return 0;
        }

        const TimeHandlerConfig timeConfig = TimeHandlerFactory::loadConfigFromEnvironment();
        const TimeHandler timeHandler = TimeHandlerFactory::create(timeConfig);

        HistoricalCsvSource source(options.historical_data);
        HistoricalOpenCsvSource openSource(options.historical_data);
        MarketDataStore store(options.market_data_db);
        NatsJetStreamMessageBus bus(options.nats_url);
        bus.ensureStream(options.stream, TransportSubjects::tradingRuntimeSubjects());

        const TrackedMarketData existing = store.loadTracked();
        const Timestamp checkpoint = trackedDate(existing);
        const Timestamp newestAtStartup = newestCompletedBusinessUtcDate(timeHandler);

        if (checkpoint > newestAtStartup) {
            throw std::runtime_error(
                "Canonical replay SQLite is ahead of TimeHandler business time: checkpoint=" +
                std::to_string(checkpoint) + " newest_completed=" +
                std::to_string(newestAtStartup));
        }

        if (checkpoint != 0) {
            // Re-read and republish the durable checkpoint date on restart. If a crash
            // happened after SQLite COMMIT but before NATS publish, this closes that gap;
            // if the event was already published, its deterministic message ID dedupes it.
            const auto checkpointDay = source.skipThrough(checkpoint);
            if (!checkpointDay.has_value() || checkpointDay->date != checkpoint)
                throw std::logic_error("Historical source/checkpoint recovery mismatch");
            commitAndPublish(store, bus, options, *checkpointDay);
        }

        LG_INFO(
            "service=historical-market-data event=service_ready source={} dataset={} database={} stream={} market_top_n={} poll_timeout_ms={} checkpoint={} time_speed={} time_identity={}",
            options.source_id,
            options.historical_data.string(),
            options.market_data_db.string(),
            options.stream,
            options.market_top_n,
            options.poll_timeout_ms,
            checkpoint,
            timeConfig.speed,
            timeConfig.identity()
        );

        std::optional<HistoricalMarketDay> pending;

        while (running.load()) {
            // The open of UTC day D is visible when business time reaches D, even
            // though D's high/low/close/volume are not.  This independent cursor
            // releases only date/symbol/open and therefore preserves anti-lookahead.
            const Timestamp businessDate = currentBusinessUtcDate(timeHandler);
            while (running.load()) {
                const auto nextOpen = openSource.nextDate();
                if (!nextOpen.has_value() || *nextOpen > businessDate)
                    break;
                publishExecutionOpen(bus, openSource.readNextDay());
            }

            if (!pending.has_value()) {
                const auto next = source.nextDate();
                if (!next.has_value()) {
                    LG_INFO("service=historical-market-data event=historical_source_exhausted");
                    return 0;
                }

                const Timestamp newestCompleted = newestCompletedBusinessUtcDate(timeHandler);
                if (*next > newestCompleted) {
                    if (options.run_once)
                        return 0;
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(options.poll_timeout_ms));
                    continue;
                }

                pending = source.readNextDay();
                if (pending->date > newestCompleted)
                    throw std::logic_error("Historical source exposed a future business day");
            }

            try {
                commitAndPublish(store, bus, options, *pending);
                pending.reset();
            }
            catch (const std::exception& error) {
                LG_ERROR(
                    "service=historical-market-data event=day_release_failed date={} retry_ms={} time_domain=technical error={}",
                    pending->date,
                    options.poll_timeout_ms,
                    error.what()
                );
                if (options.run_once)
                    return 1;
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(options.poll_timeout_ms));
            }
        }

        LG_INFO("service=historical-market-data event=shutdown_complete");
        return 0;
    }
    catch (const std::exception& error) {
        LG_ALERT("service=historical-market-data event=fatal error={}", error.what());
        return 1;
    }
}
