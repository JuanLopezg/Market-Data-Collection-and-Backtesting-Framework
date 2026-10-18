#include "paper_baseline.h"

#include <fstream>
#include <set>
#include <unordered_map>
#include <nlohmann/json.hpp>

#include "account.h"
#include "adapters/simulated_exchange.h"
#include "canonical_market_data_reader.h"
#include "chain_profile.h"
#include "trade_recorder.h"
#include "trading_engine.h"

namespace {
using json = nlohmann::json;
using chain_profile::addSizerHistory;
using chain_profile::readJson;
using chain_profile::sizerHistoryDays;
}

int runPaperBaseline(const std::string& manifestPath)
{
    const auto input = readJson(manifestPath);
    const auto strategy = readJson(input.at("strategy_config")).at("strategies").at(0);
    const auto portfolio = readJson(input.at("portfolio_config")).at("strategies").at(0);
    StrategyPortfolio strategies = chain_profile::makeStrategyPortfolio(strategy, portfolio);

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
        // Kraken LIVE studies (absent for PAPER): USD deposited or withdrawn since the
        // previous cycle enters the expected cash too, so it is never a difference; the
        // day's BTC collateral after haircut is extra sizing capital, as the risk service
        // uses it.
        account.applyCashMovement(cycle.value("capital_flow_usd", 0.0));
        const double collateral = cycle.value("collateral_usd", 0.0);
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
        // Signals see only today's top-50 entry universe (strategy service). Sizing and
        // rebalance prices also need close(T) of every signalled or held coin, as the risk
        // service loads them; otherwise a held coin that left the top 50 has no price.
        MarketData sizingData = marketData;
        CoinBarMap& todayBars = sizingData[closeDate];
        for (const Coin& coin : held) {
            const auto bars = window.raw_data.data.find(coin);
            if (bars == window.raw_data.data.end()) continue;
            const auto found = bars->second.find(closeDate);
            if (found == bars->second.end()) continue;
            BarData bar;
            bar.open = found->second.open;
            bar.high = found->second.high;
            bar.low = found->second.low;
            bar.close = found->second.close;
            bar.volume = found->second.volume;
            bar.quote_volume = found->second.quote_volume;
            todayBars.emplace(coin, bar); // A top-50 coin keeps its existing bar.
        }
        engine.onBarClose(marketData, sizingData, closeDate, closes, collateral);
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
        double fees = 0.0; // Commission of every expected trade so far (open and closed).
        for (const TradeRecord& trade : recorder.allTrades(opens, openDate))
            fees += trade.commission;
        rows.push_back({{"decisionDate", closeDate}, {"executionDate", openDate},
            {"cash", account.cash()}, {"equity", account.equity(opens)},
            {"fees", fees},
            {"positions", account.positions().values()},
            {"signals", strategies.front().signalState().values()}});
    }
    std::ofstream output(input.at("output").get<std::string>());
    if (!output) throw std::runtime_error("Cannot write PAPER baseline output");
    output << json{{"contractVersion", "paper-fast-v1"}, {"rows", rows}}.dump(2) << '\n';
    return 0;
}
