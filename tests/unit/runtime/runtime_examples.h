#pragma once

#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <set>
#include <vector>

#include <gtest/gtest.h>

#include "adapters/simulated_exchange.h"
#include "entry_exit_only_rebalance_policy.h"
#include "equal_weight_sizer.h"
#include "recovery_coordinator.h"
#include "replay_runtime.h"
#include "sqlite_state_store.h"
#include "test_support.h"
#include "trading_engine.h"
#include "validated/pure_rsi.h"

namespace runtime_test {

inline Timestamp openTime(unsigned day) { return (20260100U + day) * 10U + 1U; }
inline Timestamp closeTime(unsigned day) { return (20260100U + day) * 10U + 9U; }
inline Timestamp manualTime(unsigned day) { return (20260100U + day) * 10U + 5U; }

struct StrategyConfig {
    StrategyID id = 7;
    double allocation = 1.0;
    double entry_weight = 0.25;
    double max_gross = 1.0;
    double max_asset = 1.0;
};

inline std::unique_ptr<Strategy> pureRsi()
{
    return std::make_unique<StrategyPureRSI>(2, nullptr, nullptr, 2, 2, 60.0, 40.0);
}

inline StrategyPortfolio tradingStrategies(const std::vector<StrategyConfig>& configs)
{
    StrategyPortfolio strategies;
    for (const auto& config : configs)
        strategies.emplace_back(config.id, pureRsi(), config.allocation,
            std::make_unique<EqualWeightSizer>(config.entry_weight),
            RiskConstraints(config.max_gross, config.max_asset),
            std::make_unique<EntryExitOnlyRebalancePolicy>());
    return strategies;
}

inline StrategySignalPortfolio signalStrategies(const std::vector<StrategyConfig>& configs)
{
    StrategySignalPortfolio strategies;
    for (const auto& config : configs) strategies.emplace_back(config.id, pureRsi());
    return strategies;
}

inline std::vector<PortfolioRiskStrategyConfig> riskConfigs(const std::vector<StrategyConfig>& configs)
{
    std::vector<PortfolioRiskStrategyConfig> strategies;
    for (const auto& config : configs)
        strategies.emplace_back(config.id, "Pure_RSI", config.allocation,
            std::make_unique<EqualWeightSizer>(config.entry_weight),
            RiskConstraints(config.max_gross, config.max_asset),
            std::make_unique<EntryExitOnlyRebalancePolicy>());
    return strategies;
}

inline std::vector<StrategyID> strategyIds(const std::vector<StrategyConfig>& configs)
{
    std::vector<StrategyID> ids;
    for (const auto& config : configs) ids.push_back(config.id);
    return ids;
}

inline MarketSliceSnapshot daySlice(unsigned day, double fourthClose = 100.0, double fifthClose = 90.0)
{
    const std::array<double, 5> opens{100.0, 105.0, 115.0, 125.0, 150.0};
    const std::array<double, 5> closes{100.0, 110.0, 120.0, fourthClose, fifthClose};
    const double open = opens.at(day - 1);
    const double close = closes.at(day - 1);
    return {{}, closeTime(day), {{"BTCUSDT", OHLCV{
        open, std::max(open, close) + 5.0, std::min(open, close) - 5.0, close, 10000.0}}}};
}

inline RollingMarketState marketHistory(double fourthClose = 100.0, double fifthClose = 90.0)
{
    RollingMarketState history;
    for (unsigned day = 1; day <= 5; ++day) history.append(daySlice(day, fourthClose, fifthClose));
    return history;
}

inline PriceSnapshot prices(double btc, double eth = 100.0)
{
    PriceSnapshot result;
    result.set("BTCUSDT", btc);
    result.set("ETHUSDT", eth);
    return result;
}

struct TradingRig {
    RollingMarketState history;
    IndicatorEngine indicators;
    StrategyPortfolio strategies;
    Account account{1000.0};
    TradeRecorder recorder;
    SimulatedExchange exchange;
    TradingEngine engine;

    explicit TradingRig(const std::vector<StrategyConfig>& configs = {StrategyConfig{}},
                        double commission = 0.0, double fourthClose = 100.0)
        : history(marketHistory(fourthClose)), strategies(tradingStrategies(configs)),
          exchange(commission), engine(strategies, account, recorder, indicators, exchange)
    {
        indicators.precompute(history.rawData(), {{IndicatorKind::RSI, PriceField::Close, 2}});
    }

    void close(unsigned day, double extraCapital = 0.0)
    {
        engine.onBarClose(history.marketData(), closeTime(day), history.closePrices(closeTime(day)), extraCapital);
    }

    void open(unsigned day)
    {
        engine.executePendingPlans(openTime(day), history.openPrices(closeTime(day)));
        exchange.processOpen(openTime(day), history.marketData().at(closeTime(day)));
        engine.processExchangeEvents();
    }

    void completeDay(unsigned day) { open(day); close(day); }
    void through(unsigned day) { for (unsigned current = 1; current <= day; ++current) completeDay(current); }
};

inline void expectAccount(const TradingRig& rig, double cash, double quantity)
{
    EXPECT_NEAR(rig.account.cash(), cash, 1e-10);
    EXPECT_NEAR(rig.account.positions().get("BTCUSDT"), quantity, 1e-12);
    double attributed = 0.0;
    for (const auto& strategy : rig.strategies) attributed += strategy.virtualPositions().get("BTCUSDT");
    EXPECT_NEAR(attributed, quantity, 1e-12);
}

inline MockVenue::MockChaosConfig chaosConfig()
{
    MockVenue::MockChaosConfig config;
    config.submit_limit = config.cancel_limit = config.modify_limit = 10000;
    config.market_source_limit = config.reconcile_limit = config.stream_read_limit = 10000;
    return config;
}

inline MockVenue::MatchingFillConfig matchingConfig()
{
    MockVenue::MatchingFillConfig config;
    config.ignore_volume_capacity = true;
    config.max_adverse_slippage_ppm = 0;
    config.fee_ppm = 0;
    return config;
}

inline MockVenue::MockAccountingConfig accountingConfig()
{
    MockVenue::MockAccountingConfig config;
    config.initial_cash = "1000.00";
    config.fee_ppm = 0;
    return config;
}

inline void matchAtOpen(MockVenue::MockExchange& venue, Timestamp at, const PriceSnapshot& opens,
                        const std::vector<Coin>& coins = {"BTCUSDT"}, double volume = 10000.0)
{
    for (const auto& coin : coins) {
        const double open = opens.get(coin);
        MockVenue::MarketBarObservation observation{coin, at, OHLCV{open, open, open, open, volume}};
        const auto result = venue.processMarketBar(observation);
        ASSERT_EQ(result.status, MockVenue::ChaosStatus::Delivered);
    }
}

struct ReplayRig {
    RollingMarketState history;
    StrategySignalEngine signals;
    PortfolioRiskEngine risk;
    NotionalOrderPlanner planner;
    MockVenue::MockExchange venue;
    Replay::ReplayRuntime runtime;

    explicit ReplayRig(const std::filesystem::path& directory, bool parity = true,
                       const std::vector<StrategyConfig>& configs = {StrategyConfig{}},
                       double fourthClose = 100.0)
        : history(marketHistory(fourthClose)), signals(signalStrategies(configs)), risk(riskConfigs(configs)),
          venue(directory, chaosConfig(), matchingConfig(), accountingConfig()),
          runtime(signals, risk, planner, strategyIds(configs), venue, true, parity)
    {}

    void close(unsigned day)
    {
        runtime.onClosedSlice(daySlice(day, history.marketData().at(closeTime(4)).at("BTCUSDT").close));
    }

    void open(unsigned day, double volume = 10000.0)
    {
        const auto opens = history.openPrices(closeTime(day));
        runtime.onExecutionOpen(openTime(day), opens);
        matchAtOpen(venue, openTime(day), opens, {"BTCUSDT"}, volume);
    }

    void completeDay(unsigned day) { open(day); close(day); }
    void through(unsigned day) { for (unsigned current = 1; current <= day; ++current) completeDay(current); }
    double quantity() const { return runtime.strategyPositions().at(7).get("BTCUSDT"); }
};

inline void expectFillsEqual(const std::vector<Fill>& actual, const std::vector<Fill>& expected)
{
    ASSERT_EQ(actual.size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const auto& a = actual[index];
        const auto& b = expected[index];
        EXPECT_EQ(a.fill_id, b.fill_id);
        EXPECT_EQ(a.order_id, b.order_id);
        EXPECT_EQ(a.strategy_id, b.strategy_id);
        EXPECT_EQ(a.timestamp, b.timestamp);
        EXPECT_EQ(a.coin, b.coin);
        EXPECT_EQ(a.side, b.side);
        EXPECT_EQ(a.quantity, b.quantity);
        EXPECT_EQ(a.price, b.price);
        EXPECT_EQ(a.commission, b.commission);
    }
}

inline void expectTradingStatesEqual(const TradingStateSnapshot& actual, const TradingStateSnapshot& expected)
{
    EXPECT_EQ(actual.schema_version, expected.schema_version);
    EXPECT_EQ(actual.account_cash, expected.account_cash);
    EXPECT_EQ(actual.account_positions, expected.account_positions);
    EXPECT_EQ(actual.next_order_id, expected.next_order_id);
    EXPECT_EQ(actual.last_bar_close_timestamp, expected.last_bar_close_timestamp);
    EXPECT_EQ(actual.last_execution_timestamp, expected.last_execution_timestamp);
    EXPECT_EQ(std::set<FillID>(actual.processed_fill_ids.begin(), actual.processed_fill_ids.end()),
              std::set<FillID>(expected.processed_fill_ids.begin(), expected.processed_fill_ids.end()));
    ASSERT_EQ(actual.strategies.size(), expected.strategies.size());
    for (const auto& state : expected.strategies) {
        const auto found = std::find_if(actual.strategies.begin(), actual.strategies.end(),
            [&](const auto& candidate) { return candidate.strategy_id == state.strategy_id; });
        ASSERT_NE(found, actual.strategies.end());
        EXPECT_EQ(found->signals, state.signals);
        EXPECT_EQ(found->desired_weights, state.desired_weights);
        EXPECT_EQ(found->virtual_positions, state.virtual_positions);
    }
    ASSERT_EQ(actual.orders.size(), expected.orders.size());
    for (const auto& order : expected.orders) {
        const auto found = std::find_if(actual.orders.begin(), actual.orders.end(),
            [&](const auto& candidate) { return candidate.request.order_id == order.request.order_id; });
        ASSERT_NE(found, actual.orders.end());
        EXPECT_EQ(found->request.strategy_id, order.request.strategy_id);
        EXPECT_EQ(found->request.coin, order.request.coin);
        EXPECT_EQ(found->request.side, order.request.side);
        EXPECT_EQ(found->request.quantity, order.request.quantity);
        EXPECT_EQ(found->status, order.status);
        EXPECT_EQ(found->filled_quantity, order.filled_quantity);
    }
    ASSERT_EQ(actual.pending_plans.size(), expected.pending_plans.size());
    for (const auto& pending : expected.pending_plans) {
        const auto found = std::find_if(actual.pending_plans.begin(), actual.pending_plans.end(),
            [&](const auto& candidate) { return candidate.strategy_id == pending.strategy_id; });
        ASSERT_NE(found, actual.pending_plans.end());
        EXPECT_EQ(found->plan.timestamp(), pending.plan.timestamp());
        EXPECT_EQ(found->plan.referenceCapital(), pending.plan.referenceCapital());
        ASSERT_EQ(found->plan.size(), pending.plan.size());
        for (const auto& [coin, decision] : pending.plan.values()) {
            EXPECT_EQ(found->plan.get(coin).action, decision.action);
            EXPECT_EQ(found->plan.get(coin).target_weight, decision.target_weight);
        }
    }
}

} // namespace runtime_test
