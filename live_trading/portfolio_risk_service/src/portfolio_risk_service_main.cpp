#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

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
    int poll_timeout_ms = 250;
};

void printUsage()
{
    std::cout
        << "Usage: algotrading_portfolio_risk_service [options]\n"
        << "  --nats-url URL\n"
        << "  --stream NAME\n"
        << "  --portfolio-config PATH\n"
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
    RollingMarketState market_state_;
    PortfolioRiskEngine engine_;
    std::map<Timestamp, AccountSnapshot> account_snapshots_;

    DurableMessageBus::SubscriptionID market_subscription_ = 0;
    DurableMessageBus::SubscriptionID account_subscription_ = 0;
    DurableMessageBus::SubscriptionID strategy_subscription_ = 0;

    void pruneAccountSnapshots()
    {
        while (account_snapshots_.size() > 128)
            account_snapshots_.erase(account_snapshots_.begin());
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

            const bool appended = market_state_.append(slice);
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
            account_snapshots_[snapshot.timestamp] = std::move(snapshot);
            pruneAccountSnapshots();
            LG_INFO(
                "service=portfolio-risk event=account_snapshot_buffered timestamp={} cash={} physical_positions={} strategy_position_sets={} buffered_snapshots={}",
                snapshotTimestamp,
                cash,
                physicalPositions,
                strategyPositionSets,
                account_snapshots_.size()
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
            if (signals.timestamp <= engine_.lastTimestamp()) {
                LG_INFO(
                    "service=portfolio-risk event=strategy_intents_duplicate timestamp={} last_timestamp={} disposition=ack",
                    signals.timestamp,
                    engine_.lastTimestamp()
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

            DecisionBatch output = engine_.onSignals(
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

            bus_.publish(
                TransportSubjects::DECISION_BATCH,
                ContractJsonCodec::encode(output),
                output.metadata.message_id
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
          bus_(options_.nats_url),
          engine_(loadPortfolioConfig(options_.portfolio_config))
    {
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
            "service=portfolio-risk event=service_ready stream={} config={} poll_timeout_ms={}",
            options_.stream,
            options_.portfolio_config,
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
