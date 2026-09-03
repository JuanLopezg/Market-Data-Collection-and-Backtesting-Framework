#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "account.h"
#include "contract_json_codec.h"
#include "database_utils.h"
#include "decision_engine.h"
#include "entry_exit_only_rebalance_policy.h"
#include "equal_weight_sizer.h"
#include "sample_covariance_estimator.h"
#include "execution_engine.h"
#include "execution_reference_prices.h"
#include "indicator_engine.h"
#include "indicator_ranker.h"
#include "liquidity_universe.h"
#include "nats_jetstream_message_bus.h"
#include "pureRSI.h"
#include "realtest.h"
#include "rolling_market_state.h"
#include "simulated_exchange.h"
#include "strategy_instance.h"
#include "threshold_rebalance_policy.h"
#include "trade_recorder.h"
#include "volatility_target_sizer.h"
#include "transport_subjects.h"

namespace {

constexpr double EPS = 1e-9;

struct Options {
    std::string nats_url = "nats://127.0.0.1:4222";
    std::string stream = "ALGOTRADING_RUNTIME";
    std::size_t expected_cycles = 0;
    double initial_cash = 100000.0;
    double commission_rate = 0.0;
    int timeout_seconds = 30;
    bool require_trading = false;
    std::string portfolio_mode = "equal-weight";
    std::string realtest_csv;
    std::string realtest_comparison_csv;
    std::string historical_data;
};

[[noreturn]] void fail(Timestamp timestamp, const std::string& layer, const std::string& detail)
{
    std::ostringstream out;
    out << "first divergence";
    if (timestamp != 0)
        out << " @ " << timestamp;
    out << " [" << layer << "]: " << detail;
    throw std::runtime_error(out.str());
}

bool nearlyEqual(double a, double b, double eps = EPS)
{
    if (a == b)
        return true;
    if (!std::isfinite(a) || !std::isfinite(b))
        return false;
    const double scale = std::max({1.0, std::abs(a), std::abs(b)});
    return std::abs(a - b) <= eps * scale;
}

Options parseOptions(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](const char* name) -> std::string {
            if (i + 1 >= argc)
                throw std::invalid_argument(std::string(name) + " requires a value");
            return argv[++i];
        };

        if (arg == "--nats-url") options.nats_url = value("--nats-url");
        else if (arg == "--stream") options.stream = value("--stream");
        else if (arg == "--expected-cycles") options.expected_cycles = std::stoull(value("--expected-cycles"));
        else if (arg == "--initial-cash") options.initial_cash = std::stod(value("--initial-cash"));
        else if (arg == "--commission-rate") options.commission_rate = std::stod(value("--commission-rate"));
        else if (arg == "--timeout-seconds") options.timeout_seconds = std::stoi(value("--timeout-seconds"));
        else if (arg == "--require-trading") options.require_trading = true;
        else if (arg == "--portfolio-mode") options.portfolio_mode = value("--portfolio-mode");
        else if (arg == "--realtest-csv") options.realtest_csv = value("--realtest-csv");
        else if (arg == "--realtest-comparison-csv") options.realtest_comparison_csv = value("--realtest-comparison-csv");
        else if (arg == "--historical-data") options.historical_data = value("--historical-data");
        else throw std::invalid_argument("Unknown option: " + arg);
    }

    if (options.expected_cycles == 0)
        throw std::invalid_argument("--expected-cycles must be positive");
    if (!std::isfinite(options.initial_cash) || options.initial_cash <= 0.0)
        throw std::invalid_argument("--initial-cash must be finite and positive");
    if (!std::isfinite(options.commission_rate) || options.commission_rate < 0.0)
        throw std::invalid_argument("--commission-rate must be finite and non-negative");
    if (options.timeout_seconds <= 0)
        throw std::invalid_argument("--timeout-seconds must be positive");
    if (options.portfolio_mode != "equal-weight" && options.portfolio_mode != "vol-target")
        throw std::invalid_argument("--portfolio-mode must be equal-weight or vol-target");
    if (!options.realtest_csv.empty() && options.historical_data.empty())
        throw std::invalid_argument("--realtest-csv requires --historical-data so open campaigns can be marked exactly like research");
    return options;
}


PortfolioSizerKind portfolioSizerKind(const std::string& portfolioMode)
{
    if (portfolioMode == "equal-weight")
        return PortfolioSizerKind::EqualWeight;
    if (portfolioMode == "vol-target")
        return PortfolioSizerKind::VolatilityTarget;
    throw std::invalid_argument("Unsupported portfolio mode: " + portfolioMode);
}

std::map<TradeID, Trade> legacyTradesForResearch(
    const TradeRecorder& recorder,
    const PriceSnapshot& marks,
    Timestamp timestamp
)
{
    std::map<TradeID, Trade> result;
    const auto records = recorder.allTrades(marks, timestamp);

    // Deliberately mirror BacktestContext::GetTradesHistory() field-for-field.
    // RealTest therefore sees the same legacy Trade representation that research uses.
    for (const TradeRecord& record : records) {
        Trade trade;
        trade.trade_id_ = record.trade_id;
        trade.start_ = record.start;
        trade.end_ = record.end;
        trade.commission_ = record.commission;
        trade.coin_ = record.coin;
        trade.direction_ = record.direction;
        trade.current_price_ = record.exit_price;
        trade.entry_ = record.entry_price;
        trade.exit_ = record.exit_price;
        trade.size_ = record.peak_quantity;
        trade.pnl_ = record.pnl;
        trade.isSimulated_ = false;
        trade.exited_ = record.exited;
        trade.strategy_name_ = record.strategy_name;
        result[trade.trade_id_] = std::move(trade);
    }

    return result;
}

std::pair<Timestamp, PriceSnapshot> finalResearchMarks(const std::string& historicalData)
{
    const OHLCVData raw = loadDatabase(std::filesystem::path(historicalData), 0);
    if (raw.data.empty())
        throw std::runtime_error("RealTest bridge could not load historical data");

    Timestamp finalTimestamp = 0;
    for (const auto& [coin, series] : raw.data) {
        (void)coin;
        if (!series.empty())
            finalTimestamp = std::max(finalTimestamp, series.rbegin()->first);
    }
    if (finalTimestamp == 0)
        throw std::runtime_error("RealTest bridge historical data has no timestamp");

    PriceSnapshot marks;
    for (const auto& [coin, series] : raw.data) {
        const auto it = series.find(finalTimestamp);
        if (it != series.end() && std::isfinite(it->second.close) && it->second.close > 0.0)
            marks.set(coin, it->second.close);
    }
    return {finalTimestamp, marks};
}

void compareDistributedWithRealtestResearchPolicy(
    const Options& options,
    const TradeRecorder& distributedRecorder
)
{
    if (options.realtest_csv.empty())
        return;

    if (!std::filesystem::exists(options.realtest_csv))
        throw std::runtime_error("RealTest CSV not found: " + options.realtest_csv);

    const auto [finalTimestamp, marks] = finalResearchMarks(options.historical_data);
    std::map<TradeID, Trade> trades = legacyTradesForResearch(
        distributedRecorder,
        marks,
        finalTimestamp
    );

    std::filesystem::path realtestPath(options.realtest_csv);
    std::filesystem::path comparisonPath = options.realtest_comparison_csv.empty()
        ? realtestPath.parent_path() / "distributed_realtest_comparison.csv"
        : std::filesystem::path(options.realtest_comparison_csv);

    std::cout << "\n============================================================\n";
    std::cout << "DISTRIBUTED -> REALTEST USING EXACT RESEARCH POLICY\n";
    std::cout << "============================================================\n";
    std::cout << "RealTest CSV     : " << realtestPath.string() << "\n";
    std::cout << "Historical marks : " << options.historical_data << " @ " << finalTimestamp << "\n";
    std::cout << "Distributed trades presented to research comparator: " << trades.size() << "\n";

    // IMPORTANT: this is the same public entry point called by research/backtesting_main.cpp.
    // We intentionally do not reinterpret its tolerances, matching rules or VolTarget policy.
    const bool exactResearchResult = compareBacktestBySizing(
        portfolioSizerKind(options.portfolio_mode),
        realtestPath,
        trades,
        comparisonPath
    );

    // Research itself does not fail the executable on a comparison mismatch; it displays
    // the comparison for manual validation. Preserve that behavior here so known, already
    // accepted RealTest differences are not silently turned into a new policy.
    std::cout << "DISTRIBUTED_REALTEST_RESEARCH_POLICY: "
              << (exactResearchResult ? "MATCH" : "REVIEW_REQUIRED") << "\n";
}

StrategyPortfolio makeFastStrategies(const std::string& portfolioMode)
{
    std::unique_ptr<PortfolioSizer> sizer;
    std::unique_ptr<RebalancePolicy> rebalancePolicy;

    if (portfolioMode == "equal-weight") {
        sizer = std::make_unique<EqualWeightSizer>(0.10);
        rebalancePolicy = std::make_unique<EntryExitOnlyRebalancePolicy>();
    }
    else if (portfolioMode == "vol-target") {
        sizer = std::make_unique<VolatilityTargetSizer>(
            std::make_unique<SampleCovarianceEstimator>(5, 365.0),
            0.20
        );
        rebalancePolicy = std::make_unique<ThresholdRebalancePolicy>(0.02);
    }
    else {
        throw std::invalid_argument("Unsupported fast portfolio mode: " + portfolioMode);
    }

    StrategyPortfolio result;
    result.emplace_back(
        1,
        std::make_unique<StrategyPureRSI>(
            10,
            std::make_unique<TopNLiquidityUniverse>(
                IndicatorSpec{IndicatorKind::SMA, PriceField::Volume, 25, 0},
                20,
                true,
                true
            ),
            std::make_unique<IndicatorRanker>(
                IndicatorSpec{IndicatorKind::RSI, PriceField::Close, 7, 0},
                true,
                true
            ),
            1000000,
            7,
            80.0,
            70.0
        ),
        1.0,
        std::move(sizer),
        RiskConstraints(1.50, 1.50),
        std::move(rebalancePolicy)
    );
    return result;
}

std::vector<IndicatorSpec> requiredIndicators(const StrategyPortfolio& strategies)
{
    std::vector<IndicatorSpec> result;
    for (const auto& strategy : strategies) {
        const auto specs = strategy.strategy().requiredIndicators();
        result.insert(result.end(), specs.begin(), specs.end());
    }
    return result;
}

struct Captured {
    std::map<Timestamp, MarketSliceSnapshot> market_slices;
    std::map<Timestamp, StrategyIntentBatch> strategy_intents;
    std::map<Timestamp, DecisionBatch> decisions;
    std::map<Timestamp, ExecutionPriceSnapshot> prices_by_decision;
    std::vector<SubmitOrderCommand> submits;
    std::vector<CancelOrderCommand> cancels;
    std::vector<Fill> fills;
    std::vector<AccountSnapshot> account_snapshots;
    std::map<Timestamp, ExecutionCycleComplete> cycles;
};

DurableConsumerOptions wildcardConsumer(const Options& options)
{
    DurableConsumerOptions value;
    value.stream = options.stream;
    value.durable_name = "distributed-fast-compare";
    value.subject = ">";
    value.ack_wait_ms = 30000;
    value.max_deliver = 5;
    value.max_ack_pending = 2048;
    return value;
}

Captured captureRuntime(const Options& options)
{
    NatsJetStreamMessageBus bus(options.nats_url);
    Captured result;

    const auto subscription = bus.subscribe(
        wildcardConsumer(options),
        [&](const BusMessage& message) {
            if (message.subject == TransportSubjects::MARKET_SLICE_SNAPSHOT) {
                auto value = ContractJsonCodec::decodeMarketSliceSnapshot(message.payload);
                result.market_slices[value.timestamp] = std::move(value);
            } else if (message.subject == TransportSubjects::STRATEGY_INTENTS) {
                auto value = ContractJsonCodec::decodeStrategyIntentBatch(message.payload);
                result.strategy_intents[value.timestamp] = std::move(value);
            } else if (message.subject == TransportSubjects::DECISION_BATCH) {
                auto value = ContractJsonCodec::decodeDecisionBatch(message.payload);
                result.decisions[value.decision_timestamp] = std::move(value);
            } else if (message.subject == TransportSubjects::EXECUTION_PRICES) {
                auto value = ContractJsonCodec::decodeExecutionPriceSnapshot(message.payload);
                result.prices_by_decision[value.decision_timestamp] = std::move(value);
            } else if (message.subject == TransportSubjects::SUBMIT_ORDER) {
                result.submits.push_back(ContractJsonCodec::decodeSubmitOrderCommand(message.payload));
            } else if (message.subject == TransportSubjects::CANCEL_ORDER) {
                result.cancels.push_back(ContractJsonCodec::decodeCancelOrderCommand(message.payload));
            } else if (message.subject == TransportSubjects::FILL) {
                result.fills.push_back(ContractJsonCodec::decodeFillEvent(message.payload).fill);
            } else if (message.subject == TransportSubjects::ACCOUNT_SNAPSHOT) {
                result.account_snapshots.push_back(ContractJsonCodec::decodeAccountSnapshot(message.payload));
            } else if (message.subject == TransportSubjects::EXECUTION_CYCLE_COMPLETE) {
                auto value = ContractJsonCodec::decodeExecutionCycleComplete(message.payload);
                result.cycles[value.decision_timestamp] = std::move(value);
            }
            return DurableMessageDisposition::Ack;
        }
    );

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(options.timeout_seconds);
    int quietRounds = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const std::size_t count = bus.poll(subscription, 256, 100);
        if (result.cycles.size() >= options.expected_cycles && count == 0)
            ++quietRounds;
        else
            quietRounds = 0;
        if (result.cycles.size() >= options.expected_cycles && quietRounds >= 5)
            break;
    }
    bus.close(subscription);

    if (result.cycles.size() != options.expected_cycles)
        throw std::runtime_error("retained runtime did not contain expected execution cycles");
    if (result.market_slices.size() != options.expected_cycles)
        throw std::runtime_error("retained runtime did not contain expected market slices");
    if (result.strategy_intents.size() != options.expected_cycles)
        throw std::runtime_error("retained runtime did not contain expected strategy batches");
    if (result.decisions.size() != options.expected_cycles)
        throw std::runtime_error("retained runtime did not contain expected decision batches");
    if (result.prices_by_decision.size() != options.expected_cycles)
        throw std::runtime_error("retained runtime did not contain expected execution prices");

    return result;
}

template <typename Map>
void compareNumericMap(
    Timestamp ts,
    const std::string& layer,
    const Map& expected,
    const Map& actual
)
{
    std::set<typename Map::key_type> keys;
    for (const auto& [key, value] : expected) { (void)value; keys.insert(key); }
    for (const auto& [key, value] : actual) { (void)value; keys.insert(key); }

    for (const auto& key : keys) {
        const auto e = expected.find(key);
        const auto a = actual.find(key);
        const double ev = e == expected.end() ? 0.0 : e->second;
        const double av = a == actual.end() ? 0.0 : a->second;
        if (!nearlyEqual(ev, av)) {
            std::ostringstream out;
            out << key << " expected=" << ev << " actual=" << av;
            fail(ts, layer, out.str());
        }
    }
}

const StrategySignalIntent* findStrategy(const StrategyIntentBatch& batch, StrategyID id)
{
    for (const auto& value : batch.strategies)
        if (value.strategy_id == id) return &value;
    return nullptr;
}

const StrategyDecisionIntent* findStrategy(const DecisionBatch& batch, StrategyID id)
{
    for (const auto& value : batch.strategies)
        if (value.strategy_id == id) return &value;
    return nullptr;
}

void compareSignals(
    Timestamp ts,
    const StrategyPortfolio& fastStrategies,
    const StrategyIntentBatch& distributed
)
{
    if (distributed.timestamp != ts)
        fail(ts, "signals", "distributed timestamp mismatch");
    if (distributed.strategies.size() != fastStrategies.size())
        fail(ts, "signals", "strategy count mismatch");

    for (const auto& fast : fastStrategies) {
        const auto* actual = findStrategy(distributed, fast.id());
        if (!actual)
            fail(ts, "signals", "distributed batch missing strategy id " + std::to_string(fast.id()));
        if (actual->strategy_name != fast.name())
            fail(ts, "signals", "strategy name mismatch");
        compareNumericMap(ts, "signals/" + fast.name(), fast.signalState().values(), actual->signals);
    }
}

void compareDecisions(Timestamp ts, const DecisionBatch& expected, const DecisionBatch& actual)
{
    if (actual.decision_timestamp != ts || expected.decision_timestamp != ts)
        fail(ts, "decision", "decision timestamp mismatch");
    if (expected.strategies.size() != actual.strategies.size())
        fail(ts, "decision", "strategy decision count mismatch");

    for (const auto& e : expected.strategies) {
        const auto* a = findStrategy(actual, e.strategy_id);
        if (!a)
            fail(ts, "decision", "missing strategy id " + std::to_string(e.strategy_id));
        if (e.decision_timestamp != a->decision_timestamp)
            fail(ts, "decision", "strategy decision timestamp mismatch");
        if (!nearlyEqual(e.reference_capital, a->reference_capital)) {
            std::ostringstream out;
            out << "reference_capital expected=" << e.reference_capital
                << " actual=" << a->reference_capital;
            fail(ts, "decision", out.str());
        }

        std::set<Coin> coins;
        for (const auto& [coin, value] : e.decisions) { (void)value; coins.insert(coin); }
        for (const auto& [coin, value] : a->decisions) { (void)value; coins.insert(coin); }
        for (const Coin& coin : coins) {
            const auto ei = e.decisions.find(coin);
            const auto ai = a->decisions.find(coin);
            const RebalanceDecision ed = ei == e.decisions.end() ? RebalanceDecision::hold() : ei->second;
            const RebalanceDecision ad = ai == a->decisions.end() ? RebalanceDecision::hold() : ai->second;
            if (ed.action != ad.action || !nearlyEqual(ed.target_weight, ad.target_weight)) {
                std::ostringstream out;
                out << coin << " action/weight mismatch expected="
                    << static_cast<int>(ed.action) << "/" << ed.target_weight
                    << " actual=" << static_cast<int>(ad.action) << "/" << ad.target_weight;
                fail(ts, "decision", out.str());
            }
        }
    }
}

void compareAccount(
    Timestamp ts,
    const std::string& layer,
    const ExecutionEngine& fast,
    const AccountSnapshot& actual
)
{
    if (!nearlyEqual(fast.account().cash(), actual.cash)) {
        std::ostringstream out;
        out << "cash expected=" << fast.account().cash() << " actual=" << actual.cash;
        fail(ts, layer, out.str());
    }
    compareNumericMap(ts, layer + "/positions", fast.account().positions().values(), actual.positions);

    for (const auto& [strategyId, positions] : actual.strategy_positions) {
        const auto fastIt = fast.strategyPositions().find(strategyId);
        if (fastIt == fast.strategyPositions().end())
            fail(ts, layer, "unexpected strategy positions id " + std::to_string(strategyId));
        compareNumericMap(ts, layer + "/strategy_positions", fastIt->second.values(), positions);
    }
    if (actual.strategy_positions.size() != fast.strategyPositions().size())
        fail(ts, layer, "strategy position set size mismatch");
}

const AccountSnapshot* closeAccountSnapshot(const Captured& captured, Timestamp ts)
{
    const std::string expected = "account-snapshot:close:" + std::to_string(ts);
    for (const auto& value : captured.account_snapshots)
        if (value.metadata.message_id == expected) return &value;
    return nullptr;
}

const AccountSnapshot* lastAccountSnapshotAt(const Captured& captured, Timestamp ts)
{
    const AccountSnapshot* result = nullptr;
    for (const auto& value : captured.account_snapshots)
        if (value.timestamp == ts) result = &value;
    return result;
}

bool sameOrder(const ExecutionOrder& a, const ExecutionOrder& b)
{
    return a.order_id == b.order_id &&
           a.strategy_id == b.strategy_id &&
           a.created_at == b.created_at &&
           a.active_from == b.active_from &&
           a.coin == b.coin &&
           a.side == b.side &&
           nearlyEqual(a.quantity, b.quantity);
}

bool sameFill(const Fill& a, const Fill& b)
{
    return a.fill_id == b.fill_id &&
           a.order_id == b.order_id &&
           a.strategy_id == b.strategy_id &&
           a.timestamp == b.timestamp &&
           a.coin == b.coin &&
           a.side == b.side &&
           nearlyEqual(a.quantity, b.quantity) &&
           nearlyEqual(a.price, b.price) &&
           nearlyEqual(a.commission, b.commission);
}

std::string orderSideName(OrderSide side)
{
    return side == OrderSide::Buy ? "Buy" : "Sell";
}

std::string describeOrder(const ExecutionOrder& order)
{
    std::ostringstream out;
    out << "{order_id=" << order.order_id
        << ", strategy_id=" << order.strategy_id
        << ", coin=" << order.coin
        << ", side=" << orderSideName(order.side)
        << ", quantity=" << order.quantity
        << ", created_at=" << order.created_at
        << ", active_from=" << order.active_from
        << "}";
    return out.str();
}

std::string orderCountContext(std::size_t fastCount, std::size_t distributedCount)
{
    return "counts fast=" + std::to_string(fastCount) +
           " distributed=" + std::to_string(distributedCount);
}

TradeRecorder buildDistributedTradeRecorder(
    const std::vector<Fill>& distributedFills,
    const std::unordered_map<StrategyID, std::string>& strategyNames
)
{
    TradeRecorder recorder;
    std::vector<Fill> fills = distributedFills;
    std::sort(fills.begin(), fills.end(), [](const Fill& a, const Fill& b) {
        if (a.timestamp != b.timestamp) return a.timestamp < b.timestamp;
        return a.fill_id < b.fill_id;
    });
    for (const Fill& fill : fills) {
        const auto it = strategyNames.find(fill.strategy_id);
        if (it == strategyNames.end())
            fail(fill.timestamp, "trade/PnL", "fill has unknown strategy id");
        recorder.onFill(fill, it->second);
    }
    return recorder;
}

void compareTrades(
    const TradeRecorder& fastRecorder,
    const TradeRecorder& distributedRecorder
)
{
    const auto& fast = fastRecorder.closedTrades();
    const auto& dist = distributedRecorder.closedTrades();
    if (fast.size() != dist.size())
        fail(0, "trade/PnL", "closed trade count mismatch");
    if (!nearlyEqual(fastRecorder.cumulativeClosedPnl(), distributedRecorder.cumulativeClosedPnl()))
        fail(0, "trade/PnL", "cumulative closed PnL mismatch");

    for (std::size_t i = 0; i < fast.size(); ++i) {
        const auto& a = fast[i];
        const auto& b = dist[i];
        if (a.strategy_id != b.strategy_id || a.coin != b.coin ||
            a.start != b.start || a.end != b.end || a.direction != b.direction ||
            a.fill_count != b.fill_count ||
            !nearlyEqual(a.entry_price, b.entry_price) ||
            !nearlyEqual(a.exit_price, b.exit_price) ||
            !nearlyEqual(a.pnl, b.pnl) ||
            !nearlyEqual(a.commission, b.commission))
            fail(a.end, "trade/PnL", "closed trade fields mismatch");
    }
}

void runComparison(const Options& options, const Captured& captured)
{
    StrategyPortfolio strategies = makeFastStrategies(options.portfolio_mode);
    const std::vector<IndicatorSpec> required = requiredIndicators(strategies);
    std::unordered_map<StrategyID, std::string> strategyNames;
    for (const auto& strategy : strategies)
        strategyNames.emplace(strategy.id(), strategy.name());

    IndicatorEngine indicators;
    DecisionEngine decisionEngine(strategies, indicators);
    Account account(options.initial_cash);
    TradeRecorder recorder;
    SimulatedExchange exchange(options.commission_rate);
    ExecutionEngine executionEngine(strategies, account, recorder, exchange);
    RollingMarketState rolling;

    std::map<OrderID, ExecutionOrder> fastOrders;
    std::map<FillID, Fill> fastFills;

    for (const auto& [ts, slice] : captured.market_slices) {
        if (!rolling.append(slice))
            fail(ts, "market", "duplicate market slice in comparator input");

        const AccountSnapshot* closeSnapshot = closeAccountSnapshot(captured, ts);
        if (!closeSnapshot)
            fail(ts, "account/close", "missing distributed close account snapshot");
        compareAccount(ts, "account/close", executionEngine, *closeSnapshot);

        indicators.precompute(rolling.rawData(), required);
        const PriceSnapshot marks = rolling.closePrices(ts);
        const double equity = executionEngine.accountEquity(marks);

        decisionEngine.onBarClose(
            rolling.marketData(),
            ts,
            equity,
            executionEngine.strategyPositions()
        );

        const auto strategyIt = captured.strategy_intents.find(ts);
        if (strategyIt == captured.strategy_intents.end())
            fail(ts, "signals", "missing distributed strategy batch");
        compareSignals(ts, strategies, strategyIt->second);

        const auto decisionIt = captured.decisions.find(ts);
        if (decisionIt == captured.decisions.end())
            fail(ts, "decision", "missing distributed DecisionBatch");
        compareDecisions(ts, decisionEngine.pendingDecisions(), decisionIt->second);

        const auto priceIt = captured.prices_by_decision.find(ts);
        if (priceIt == captured.prices_by_decision.end())
            fail(ts, "execution price", "missing distributed execution price snapshot");
        const ExecutionPriceSnapshot& execPrices = priceIt->second;

        ExecutionReferencePrices refs;
        for (const auto& [coin, price] : execPrices.prices)
            refs.set(coin, price);

        const std::size_t ordersBefore = executionEngine.orderManager().orders().size();
        executionEngine.executeDecisionBatch(
            execPrices.timestamp,
            refs,
            decisionEngine.pendingDecisions(),
            [](const std::optional<Fill>&) {}
        );
        decisionEngine.clearPendingDecisions();

        // Accepted events are part of lifecycle but do not mutate account state.
        for (const ExchangeEvent& event : exchange.drainEvents())
            executionEngine.processExchangeEvent(event, [](const std::optional<Fill>&) {});

        CoinBarMap openBars;
        for (const auto& [coin, price] : execPrices.prices) {
            BarData bar;
            bar.open = price;
            bar.high = price;
            bar.low = price;
            bar.close = price;
            bar.volume = 0.0;
            openBars.emplace(coin, bar);
        }
        exchange.processOpen(execPrices.timestamp, openBars);

        const auto events = exchange.drainEvents();
        for (const ExchangeEvent& event : events) {
            if (const auto* fill = std::get_if<Fill>(&event))
                fastFills.emplace(fill->fill_id, *fill);
            executionEngine.processExchangeEvent(event, [](const std::optional<Fill>&) {});
        }

        if (executionEngine.orderManager().orders().size() < ordersBefore)
            fail(ts, "orders", "fast order history regressed");
        for (const auto& [orderId, tracked] : executionEngine.orderManager().orders())
            fastOrders.emplace(orderId, tracked.request);

        const AccountSnapshot* afterExecution = lastAccountSnapshotAt(captured, execPrices.timestamp);
        if (!afterExecution)
            fail(execPrices.timestamp, "account/execution", "missing distributed execution account snapshot");
        compareAccount(execPrices.timestamp, "account/execution", executionEngine, *afterExecution);
    }

    std::map<OrderID, ExecutionOrder> distributedOrders;
    for (const auto& command : captured.submits) {
        const auto [it, inserted] = distributedOrders.emplace(command.order.order_id, command.order);
        if (!inserted && !sameOrder(it->second, command.order))
            fail(command.order.active_from, "orders", "same OrderID published with conflicting payload");
    }
    // Diagnose key/payload divergence before reducing it to a global count mismatch.
    // OrderIDs are monotonic, so the first missing/unexpected key also identifies the
    // earliest lifecycle divergence and gives us an actionable active_from timestamp.
    for (const auto& [orderId, expected] : fastOrders) {
        const auto it = distributedOrders.find(orderId);
        if (it == distributedOrders.end()) {
            fail(
                expected.active_from,
                "orders",
                "distributed path missing OrderID " + std::to_string(orderId) +
                    " expected=" + describeOrder(expected) + " " +
                    orderCountContext(fastOrders.size(), distributedOrders.size())
            );
        }
        if (!sameOrder(expected, it->second)) {
            fail(
                expected.active_from,
                "orders",
                "OrderID " + std::to_string(orderId) +
                    " fields mismatch expected=" + describeOrder(expected) +
                    " actual=" + describeOrder(it->second) + " " +
                    orderCountContext(fastOrders.size(), distributedOrders.size())
            );
        }
    }
    for (const auto& [orderId, actual] : distributedOrders) {
        if (fastOrders.find(orderId) == fastOrders.end()) {
            fail(
                actual.active_from,
                "orders",
                "distributed path has unexpected OrderID " + std::to_string(orderId) +
                    " actual=" + describeOrder(actual) + " " +
                    orderCountContext(fastOrders.size(), distributedOrders.size())
            );
        }
    }
    if (fastOrders.size() != distributedOrders.size())
        fail(0, "orders", "submit order count mismatch after key comparison " +
             orderCountContext(fastOrders.size(), distributedOrders.size()));

    if (!captured.cancels.empty())
        fail(captured.cancels.front().requested_at, "orders", "standard full-fill replay unexpectedly emitted cancel command");

    std::map<FillID, Fill> distributedFills;
    for (const Fill& fill : captured.fills) {
        const auto [it, inserted] = distributedFills.emplace(fill.fill_id, fill);
        if (!inserted && !sameFill(it->second, fill))
            fail(fill.timestamp, "fills", "same FillID delivered with conflicting business payload");
    }
    if (fastFills.size() != distributedFills.size())
        fail(0, "fills", "fill count mismatch");
    for (const auto& [fillId, expected] : fastFills) {
        const auto it = distributedFills.find(fillId);
        if (it == distributedFills.end())
            fail(expected.timestamp, "fills", "distributed path missing FillID " + std::to_string(fillId));
        if (!sameFill(expected, it->second))
            fail(expected.timestamp, "fills", "FillID " + std::to_string(fillId) + " fields mismatch");
    }

    const TradeRecorder distributedRecorder = buildDistributedTradeRecorder(captured.fills, strategyNames);
    compareTrades(recorder, distributedRecorder);

    if (options.require_trading) {
        if (distributedOrders.empty())
            fail(0, "coverage", "--require-trading requested but replay produced zero submit orders");
        if (distributedFills.empty())
            fail(0, "coverage", "--require-trading requested but replay produced zero fills");
        if (recorder.closedTrades().empty())
            fail(0, "coverage", "--require-trading requested but replay produced zero closed trades");
    }

    std::cout << "[PASS] signals match fast DecisionEngine at every released close\n";
    std::cout << "[PASS] DecisionBatch actions/weights/reference capital match fast path\n";
    std::cout << "[PASS] submit orders match fast ExecutionEngine exactly\n";
    std::cout << "[PASS] fills match fast SimulatedExchange exactly\n";
    std::cout << "[PASS] close/execution cash + account/virtual positions match at every cycle\n";
    std::cout << "[PASS] closed trades + cumulative PnL reconstructed from distributed fills match fast path\n";
    std::cout << "[SUMMARY] portfolio_mode=" << options.portfolio_mode
              << " cycles=" << captured.market_slices.size()
              << " orders=" << distributedOrders.size()
              << " fills=" << distributedFills.size()
              << " closed_trades=" << recorder.closedTrades().size()
              << " closed_pnl=" << recorder.cumulativeClosedPnl() << '\n';
    std::cout << "DISTRIBUTED_FAST_COMPARE: PASS\n";

    compareDistributedWithRealtestResearchPolicy(options, distributedRecorder);
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const Options options = parseOptions(argc, argv);
        const Captured captured = captureRuntime(options);
        runComparison(options, captured);
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "DISTRIBUTED_FAST_COMPARE: FAIL: " << error.what() << '\n';
        return 1;
    }
}
