#include "paper_baseline.h"

#include <fstream>
#include <set>
#include <nlohmann/json.hpp>

#include "account.h"
#include "adapters/simulated_exchange.h"
#include "canonical_market_data_reader.h"
#include "entry_exit_only_rebalance_policy.h"
#include "equal_weight_sizer.h"
#include "indicator_ranker.h"
#include "liquidity_universe.h"
#include "trade_recorder.h"
#include "trading_engine.h"
#include "validated/pure_rsi.h"

namespace {
using json = nlohmann::json;

json readJson(const std::string& path)
{
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("Cannot open PAPER baseline input");
    return json::parse(input);
}
}

int runPaperBaseline(const std::string& manifestPath)
{
    const auto input = readJson(manifestPath);
    const auto strategy = readJson(input.at("strategy_config")).at("strategies").at(0);
    const auto portfolio = readJson(input.at("portfolio_config")).at("strategies").at(0);
    if (strategy.at("type") != "PureRSI" || portfolio.at("sizer").at("type") != "equal_weight" ||
        portfolio.at("rebalance").at("type") != "entry_exit_only" ||
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
        std::make_unique<EqualWeightSizer>(portfolio.at("sizer").at("weight_per_full_signal")),
        RiskConstraints(portfolio.at("risk").at("max_gross_leverage"),
                        portfolio.at("risk").at("max_asset_weight")),
        std::make_unique<EntryExitOnlyRebalancePolicy>());

    Account account(input.at("initial_cash"));
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
        engine.onBarClose(window.market_data, closeDate, closes);
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
