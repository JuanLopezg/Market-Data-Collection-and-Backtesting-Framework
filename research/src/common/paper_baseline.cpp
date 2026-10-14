#include "paper_baseline.h"

#include <fstream>
#include <set>
#include <unordered_map>
#include <nlohmann/json.hpp>

#include "account.h"
#include "adapters/simulated_exchange.h"
#include "canonical_market_data_reader.h"
#include "entry_exit_only_rebalance_policy.h"
#include "equal_weight_sizer.h"
#include "indicator_ranker.h"
#include "liquidity_universe.h"
#include "sample_covariance_estimator.h"
#include "threshold_rebalance_policy.h"
#include "trade_recorder.h"
#include "trading_engine.h"
#include "validated/pure_rsi.h"
#include "volatility_target_sizer.h"

namespace {
using json = nlohmann::json;

json readJson(const std::string& path)
{
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("Cannot open PAPER baseline input");
    return json::parse(input);
}

// Same sizer and rebalance construction as the PAPER portfolio-risk service
// (live_trading/portfolio_risk_service makeSizer/makeRebalancePolicy).
std::unique_ptr<PortfolioSizer> makeSizer(const json& value)
{
    const std::string type = value.at("type").get<std::string>();
    if (type == "equal_weight")
        return std::make_unique<EqualWeightSizer>(value.at("weight_per_full_signal").get<double>());
    if (type == "volatility_target")
        return std::make_unique<VolatilityTargetSizer>(
            std::make_unique<SampleCovarianceEstimator>(
                value.at("covariance_lookback").get<std::size_t>(),
                value.at("periods_per_year").get<double>()),
            value.at("target_volatility").get<double>());
    throw std::runtime_error("Unsupported PAPER baseline sizer: " + type);
}

// Days of close history the sizer needs, counted like the risk service
// (requiredMarketHistoryDays): lookback returns + 1 + 5 buffer days; 0 for equal weight.
std::size_t sizerHistoryDays(const json& sizer)
{
    if (sizer.at("type").get<std::string>() != "volatility_target")
        return 0;
    return sizer.at("covariance_lookback").get<std::size_t>() + 1U + 5U;
}

// The window's MarketData holds only the decision day. The volatility sizer needs the
// preceding closes too, as the risk service reads them: add the earlier days from the
// window's raw history. The decision day itself is left exactly as loaded.
void addSizerHistory(MarketData& marketData, const OHLCVData& raw, Timestamp closeDate, std::size_t historyDays)
{
    std::set<Timestamp> dates;
    for (const auto& [coin, bars] : raw.data)
        for (const auto& [date, bar] : bars)
            if (date < closeDate)
                dates.insert(date);
    std::vector<Timestamp> recent(dates.begin(), dates.end());
    const std::size_t keep = historyDays > 0 ? historyDays - 1U : 0U;
    if (recent.size() > keep)
        recent.erase(recent.begin(), recent.end() - static_cast<std::ptrdiff_t>(keep));
    for (const Timestamp date : recent) {
        CoinBarMap bars;
        for (const auto& [coin, history] : raw.data) {
            const auto found = history.find(date);
            if (found == history.end())
                continue;
            BarData bar; // Only the closes matter for the covariance estimate.
            bar.open = found->second.open;
            bar.high = found->second.high;
            bar.low = found->second.low;
            bar.close = found->second.close;
            bar.volume = found->second.volume;
            bar.quote_volume = found->second.quote_volume;
            bars.emplace(coin, bar);
        }
        marketData.emplace(date, std::move(bars));
    }
}

std::unique_ptr<RebalancePolicy> makeRebalancePolicy(const json& value)
{
    const std::string type = value.at("type").get<std::string>();
    if (type == "entry_exit_only")
        return std::make_unique<EntryExitOnlyRebalancePolicy>();
    if (type == "threshold")
        return std::make_unique<ThresholdRebalancePolicy>(value.at("threshold").get<double>());
    throw std::runtime_error("Unsupported PAPER baseline rebalance policy: " + type);
}
}

int runPaperBaseline(const std::string& manifestPath)
{
    const auto input = readJson(manifestPath);
    const auto strategy = readJson(input.at("strategy_config")).at("strategies").at(0);
    const auto portfolio = readJson(input.at("portfolio_config")).at("strategies").at(0);
    if (strategy.at("type") != "PureRSI" ||
        strategy.at("universe").at("indicator").at("source") != "QuoteVolume")
        throw std::runtime_error("Unsupported PAPER baseline profile");

    const auto parameters = strategy.at("parameters");
    const unsigned rsiLength = parameters.at("rsi_length");
    StrategyPortfolio strategies;
    strategies.emplace_back(
        strategy.at("id").get<unsigned>(),
        std::make_unique<StrategyPureRSI>(
            strategy.at("max_active_signals"),
            std::make_unique<TopNLiquidityUniverse>(
                IndicatorSpec{IndicatorKind::SMA, PriceField::QuoteVolume, 25},
                strategy.at("universe").at("count"), true),
            std::make_unique<IndicatorRanker>(
                IndicatorSpec{IndicatorKind::RSI, PriceField::Close, rsiLength}, true),
            strategy.at("max_ranking_position"), rsiLength,
            parameters.at("rsi_entry"), parameters.at("rsi_exit")),
        portfolio.at("allocation_weight"),
        makeSizer(portfolio.at("sizer")),
        RiskConstraints(portfolio.at("risk").at("max_gross_leverage"),
                        portfolio.at("risk").at("max_asset_weight")),
        makeRebalancePolicy(portfolio.at("rebalance")));

    Account account(input.at("initial_cash"));
    // A study that starts mid-run (after a configuration change) begins from the PAPER
    // account and strategy state at that moment instead of the initial cash.
    if (input.contains("start")) {
        const auto& start = input.at("start");
        account.restoreState(start.at("cash").get<double>(),
                             start.at("positions").get<std::unordered_map<Coin, double>>());
        bool restored = false;
        for (const auto& state : start.at("strategies")) {
            if (state.at("strategy_id").get<unsigned>() != strategy.at("id").get<unsigned>())
                continue;
            strategies.front().restoreState(
                state.at("signals").get<std::unordered_map<Coin, double>>(),
                state.at("desired_weights").get<std::unordered_map<Coin, double>>(),
                state.at("virtual_positions").get<std::unordered_map<Coin, double>>());
            restored = true;
        }
        if (!restored)
            throw std::runtime_error("PAPER baseline start state has no entry for the strategy");
    }
    TradeRecorder recorder;
    IndicatorEngine indicators;
    SimulatedExchange exchange(input.at("commission_rate"));
    TradingEngine engine(strategies, account, recorder, indicators, exchange);
    json rows = json::array();
    Timestamp previous = 0;
    for (const auto& cycle : input.at("cycles")) {
        CanonicalMarketDataReader reader(cycle.at("database"));
        const Timestamp closeDate = cycle.at("decision_date");
        const Timestamp openDate = cycle.at("execution_date");
        if (closeDate <= previous || openDate <= closeDate)
            throw std::runtime_error("Unordered PAPER baseline cycles");
        previous = closeDate;
        std::set<Coin> held;
        for (const auto& [coin, quantity] : strategies.front().signalState().values())
            if (quantity != 0.0) held.insert(coin);
        for (const auto& [coin, quantity] : account.positions().values())
            if (quantity != 0.0) held.insert(coin);
        const auto window = reader.loadWindow(closeDate, input.at("warmup_days"), 50, held);
        // Warm indicators only; never trade the warmup history or today's unfinished close.
        indicators.precompute(window.raw_data, strategies.front().strategy().requiredIndicators());
        PriceSnapshot closes;
        for (const auto& [coin, bars] : window.raw_data.data) {
            const auto found = bars.find(closeDate);
            if (found != bars.end()) closes.set(coin, found->second.close);
        }
        MarketData marketData = window.market_data;
        addSizerHistory(marketData, window.raw_data, closeDate, sizerHistoryDays(portfolio.at("sizer")));
        engine.onBarClose(marketData, closeDate, closes);
        PriceSnapshot opens;
        CoinBarMap openBars;
        for (const auto& [coin, priceValue] : cycle.at("open_prices").items()) {
            const double price = priceValue;
            opens.set(coin, price);
            BarData bar;
            bar.open = bar.high = bar.low = bar.close = price;
            openBars.emplace(coin, bar);
        }
        engine.executePendingPlans(openDate, opens);
        engine.processExchangeEvents();
        exchange.processOpen(openDate, openBars);
        engine.processExchangeEvents();
        rows.push_back({{"decisionDate", closeDate}, {"executionDate", openDate},
            {"cash", account.cash()}, {"equity", account.equity(opens)},
            {"positions", account.positions().values()},
            {"signals", strategies.front().signalState().values()}});
    }
    std::ofstream output(input.at("output").get<std::string>());
    if (!output) throw std::runtime_error("Cannot write PAPER baseline output");
    output << json{{"contractVersion", "paper-fast-v1"}, {"rows", rows}}.dump(2) << '\n';
    return 0;
}
