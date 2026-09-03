#include <atomic>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>

#include "contract_json_codec.h"
#include "indicator_ranker.h"
#include "liquidity_universe.h"
#include "nats_jetstream_message_bus.h"
#include "pureRSI.h"
#include "rolling_market_state.h"
#include "service_logging.h"
#include "strategy_signal_engine.h"
#include "strategy_signal_instance.h"
#include "transport_subjects.h"

namespace {

using json = nlohmann::json;
std::atomic<bool> running{true};

void stopHandler(int) { running.store(false); }

struct Options {
    std::string nats_url = "nats://127.0.0.1:4222";
    std::string stream = "ALGOTRADING_RUNTIME";
    std::string strategy_config = "config/strategies/pure_rsi_signal.json";
    int poll_timeout_ms = 250;
};

void printUsage()
{
    std::cout
        << "Usage: algotrading_strategy_service [options]\n"
        << "  --nats-url URL\n"
        << "  --stream NAME\n"
        << "  --strategy-config PATH\n"
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
        } else if (arg == "--strategy-config") {
            options.strategy_config = requireValue("--strategy-config");
        } else if (arg == "--poll-timeout-ms") {
            options.poll_timeout_ms = std::stoi(requireValue("--poll-timeout-ms"));
        } else {
            throw std::invalid_argument("Unknown option: " + arg);
        }
    }

    if (options.nats_url.empty() || options.stream.empty() || options.strategy_config.empty())
        throw std::invalid_argument("Strategy-service string options cannot be empty");
    if (options.poll_timeout_ms <= 0)
        throw std::invalid_argument("--poll-timeout-ms must be positive");
    return options;
}

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

class StrategyServiceRuntime {
private:
    const Options options_;
    NatsJetStreamMessageBus bus_;
    RollingMarketState market_state_;
    StrategySignalEngine engine_;
    DurableMessageBus::SubscriptionID market_subscription_ = 0;

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

            if (!market_state_.append(slice)) {
                LG_INFO(
                    "service=strategy event=market_slice_duplicate timestamp={} disposition=ack",
                    slice.timestamp
                );
                return DurableMessageDisposition::Ack;
            }

            StrategyIntentBatch output = engine_.onBarClose(
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

            bus_.publish(
                TransportSubjects::STRATEGY_INTENTS,
                ContractJsonCodec::encode(output),
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
    explicit StrategyServiceRuntime(Options options)
        : options_(std::move(options)),
          bus_(options_.nats_url),
          engine_(loadStrategies(options_.strategy_config))
    {
        bus_.ensureStream(options_.stream, TransportSubjects::runtimeSubjects());

        DurableConsumerOptions consumer;
        consumer.stream = options_.stream;
        consumer.durable_name = "strategy-service-market-slices";
        consumer.subject = TransportSubjects::MARKET_SLICE_SNAPSHOT;
        consumer.ack_wait_ms = 30000;
        consumer.max_deliver = 20;
        consumer.max_ack_pending = 64;

        market_subscription_ = bus_.subscribe(
            consumer,
            [this](const BusMessage& message) { return onMarketSlice(message); }
        );
    }

    ~StrategyServiceRuntime() { bus_.close(market_subscription_); }

    void run()
    {
        LG_INFO(
            "service=strategy event=service_ready stream={} config={} poll_timeout_ms={}",
            options_.stream,
            options_.strategy_config,
            options_.poll_timeout_ms
        );

        while (running.load())
            bus_.poll(market_subscription_, 16, options_.poll_timeout_ms);

        LG_INFO("service=strategy event=shutdown_requested");
        bus_.flush();
        LG_INFO("service=strategy event=shutdown_complete");
    }
};

} // namespace

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
