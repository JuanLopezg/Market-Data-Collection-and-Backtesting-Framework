#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

#include "contract_json_codec.h"
#include "database_utils.h"
#include "execution_price_snapshot.h"
#include "market_data_release.h"
#include "market_slice_snapshot.h"
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
    std::filesystem::path historical_data;
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
            options.stream = requireValue("--stream");
        else if (arg == "--historical-data")
            options.historical_data = requireValue("--historical-data");
        else if (arg == "--poll-timeout-ms")
            options.poll_timeout_ms = std::stoi(requireValue("--poll-timeout-ms"));
        else if (arg == "--help" || arg == "-h") {
            std::cout
                << "Market-data service options:\n"
                << "  --nats-url URL\n"
                << "  --stream NAME\n"
                << "  --historical-data PATH   CSV/SQLite historical source (required)\n"
                << "  --poll-timeout-ms N\n";
            std::exit(0);
        }
        else
            throw std::invalid_argument("Unknown option: " + arg);
    }

    if (options.stream.empty())
        throw std::invalid_argument("Market-data stream cannot be empty");
    if (options.historical_data.empty())
        throw std::invalid_argument("--historical-data is required");
    if (!std::filesystem::exists(options.historical_data))
        throw std::invalid_argument("Historical market-data path does not exist");
    if (options.poll_timeout_ms <= 0)
        throw std::invalid_argument("--poll-timeout-ms must be positive");

    return options;
}


bool validBar(const OHLCV& bar)
{
    return std::isfinite(bar.open) && bar.open > 0.0 &&
           std::isfinite(bar.high) && bar.high > 0.0 &&
           std::isfinite(bar.low) && bar.low > 0.0 &&
           std::isfinite(bar.close) && bar.close > 0.0 &&
           std::isfinite(bar.volume) && bar.volume >= 0.0;
}


class HistoricalReleaseSource {
private:
    OHLCVData history_;

public:
    explicit HistoricalReleaseSource(const std::filesystem::path& path)
        : history_(loadDatabase(path, 0, 0))
    {
        if (history_.data.empty())
            throw std::runtime_error("Historical market-data source is empty");

        LG_INFO(
            "service=market-data event=historical_source_loaded source={} instruments={}",
            path.string(),
            history_.data.size()
        );
    }

    MarketSliceSnapshot closedSlice(Timestamp timestamp) const
    {
        MarketSliceSnapshot result;
        result.timestamp = timestamp;

        for (const auto& [coin, bars] : history_.data) {
            const auto it = bars.find(timestamp);
            if (it == bars.end())
                continue;
            if (!validBar(it->second))
                throw std::runtime_error("Invalid historical OHLCV bar for " + coin);
            result.bars.push_back({coin, it->second});
        }

        if (result.bars.empty())
            throw std::out_of_range("No closed market slice for requested timestamp");
        return result;
    }

    ExecutionPriceSnapshot executionOpen(
        Timestamp timestamp,
        Timestamp decisionTimestamp
    ) const
    {
        ExecutionPriceSnapshot result;
        result.timestamp = timestamp;
        result.decision_timestamp = decisionTimestamp;

        for (const auto& [coin, bars] : history_.data) {
            const auto it = bars.find(timestamp);
            if (it == bars.end())
                continue;
            if (!std::isfinite(it->second.open) || it->second.open <= 0.0)
                throw std::runtime_error("Invalid historical execution open for " + coin);
            result.prices.emplace(coin, it->second.open);
        }

        if (result.prices.empty())
            throw std::out_of_range("No execution opens for requested timestamp");
        return result;
    }
};


class MarketDataServiceRuntime {
private:
    const Options options_;
    NatsJetStreamMessageBus bus_;
    HistoricalReleaseSource source_;
    DurableMessageBus::SubscriptionID release_subscription_ = 0;

    DurableConsumerOptions consumer() const
    {
        DurableConsumerOptions result;
        result.stream = options_.stream;
        result.durable_name = "market-data-release";
        result.subject = TransportSubjects::MARKET_DATA_RELEASE;
        result.ack_wait_ms = 30000;
        result.max_deliver = 20;
        result.max_ack_pending = 64;
        return result;
    }

    static bool validMetadata(const ContractMetadata& metadata)
    {
        return metadata.schema_version == 1 && !metadata.message_id.empty();
    }

    static ContractMetadata outputMetadata(
        std::string messageId,
        const MarketDataReleaseRequest& request
    )
    {
        ContractMetadata metadata;
        metadata.schema_version = 1;
        metadata.message_id = std::move(messageId);
        metadata.correlation_id = request.metadata.message_id;
        metadata.produced_at = request.timestamp;
        return metadata;
    }

    DurableMessageDisposition onRelease(const BusMessage& message)
    {
        try {
            const MarketDataReleaseRequest request =
                ContractJsonCodec::decodeMarketDataReleaseRequest(message.payload);

            LG_DEBUG(
                "service=market-data event=release_received timestamp={} decision_timestamp={} message_id={}",
                request.timestamp,
                request.decision_timestamp,
                request.metadata.message_id
            );

            if (!validMetadata(request.metadata) || request.timestamp == 0) {
                LG_WARN(
                    "service=market-data event=release_terminated reason=invalid_metadata timestamp={} message_id={}",
                    request.timestamp,
                    request.metadata.message_id
                );
                return DurableMessageDisposition::Terminate;
            }

            if (request.kind == MarketDataReleaseKind::ClosedSlice) {
                if (request.decision_timestamp != 0)
                    return DurableMessageDisposition::Terminate;

                MarketSliceSnapshot slice = source_.closedSlice(request.timestamp);
                slice.metadata = outputMetadata(
                    "market-slice:" + std::to_string(request.timestamp),
                    request
                );
                bus_.publish(
                    TransportSubjects::MARKET_SLICE_SNAPSHOT,
                    ContractJsonCodec::encode(slice),
                    slice.metadata.message_id
                );
                LG_INFO(
                    "service=market-data event=market_slice_published timestamp={} bars={} message_id={} correlation_id={}",
                    slice.timestamp,
                    slice.bars.size(),
                    slice.metadata.message_id,
                    slice.metadata.correlation_id
                );
                return DurableMessageDisposition::Ack;
            }

            if (request.kind == MarketDataReleaseKind::ExecutionOpen) {
                if (request.decision_timestamp == 0 ||
                    request.timestamp <= request.decision_timestamp)
                    return DurableMessageDisposition::Terminate;

                ExecutionPriceSnapshot prices = source_.executionOpen(
                    request.timestamp,
                    request.decision_timestamp
                );
                prices.metadata = outputMetadata(
                    "execution-prices:" + std::to_string(request.decision_timestamp) + ":" +
                        std::to_string(request.timestamp),
                    request
                );
                bus_.publish(
                    TransportSubjects::EXECUTION_PRICES,
                    ContractJsonCodec::encode(prices),
                    prices.metadata.message_id
                );
                LG_INFO(
                    "service=market-data event=execution_prices_published decision_timestamp={} execution_timestamp={} prices={} message_id={}",
                    prices.decision_timestamp,
                    prices.timestamp,
                    prices.prices.size(),
                    prices.metadata.message_id
                );
                return DurableMessageDisposition::Ack;
            }

            return DurableMessageDisposition::Terminate;
        }
        catch (const std::out_of_range& error) {
            LG_WARN("service=market-data event=release_rejected disposition=terminate error={}", error.what());
            return DurableMessageDisposition::Terminate;
        }
        catch (const std::invalid_argument& error) {
            LG_WARN("service=market-data event=release_invalid disposition=terminate error={}", error.what());
            return DurableMessageDisposition::Terminate;
        }
        catch (const std::exception& error) {
            LG_ERROR("service=market-data event=release_failed disposition=retry error={}", error.what());
            return DurableMessageDisposition::Retry;
        }
    }

public:
    explicit MarketDataServiceRuntime(Options options)
        : options_(std::move(options)),
          bus_(options_.nats_url),
          source_(options_.historical_data)
    {
        bus_.ensureStream(options_.stream, TransportSubjects::runtimeSubjects());
        release_subscription_ = bus_.subscribe(
            consumer(),
            [this](const BusMessage& message) { return onRelease(message); }
        );
    }

    ~MarketDataServiceRuntime()
    {
        bus_.close(release_subscription_);
    }

    void run()
    {
        LG_INFO(
            "service=market-data event=service_ready stream={} source={} poll_timeout_ms={}",
            options_.stream,
            options_.historical_data.string(),
            options_.poll_timeout_ms
        );

        while (running.load())
            bus_.poll(release_subscription_, 16, options_.poll_timeout_ms);

        LG_INFO("service=market-data event=shutdown_requested");
        bus_.flush();
        LG_INFO("service=market-data event=shutdown_complete");
    }
};

} // namespace


int main(int argc, char** argv)
{
    ServiceLogging::setup("market-data");

    try {
        std::signal(SIGINT, stopHandler);
        std::signal(SIGTERM, stopHandler);
        MarketDataServiceRuntime runtime(parseOptions(argc, argv));
        runtime.run();
        return 0;
    }
    catch (const std::exception& error) {
        LG_ALERT("service=market-data event=fatal error={}", error.what());
        return 1;
    }
}
