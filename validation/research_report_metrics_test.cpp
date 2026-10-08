#include "backtest_metrics.h"
#include "backtester.h"
#include "donchian_signal_strategy.h"
#include "alphabetical_ranker.h"
#include "equal_weight_sizer.h"
#include "entry_exit_only_rebalance_policy.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}
}

int main()
{
    // One closed campaign earns 8 after 2 in fees; another remains open.
    // The three observed bars cross a calendar gap: duration counts bars.
    MarketData market;
    for (Timestamp date : {20200101U, 20200103U, 20200104U})
        market[date]["BTC"] = BarData{};
    Trade closed;
    closed.trade_id_ = 1;
    closed.coin_ = "BTC";
    closed.start_ = 20200101;
    closed.end_ = 20200103;
    closed.entry_ = 100;
    closed.exit_ = 110;
    closed.size_ = 1;
    closed.pnl_ = 8;
    closed.commission_ = 2;
    closed.exited_ = true;
    closed.isSimulated_ = false;
    Trade open = closed;
    open.trade_id_ = 2;
    open.start_ = 20200104;
    open.end_ = 20200104;
    open.pnl_ = 5;
    open.exited_ = false;
    BacktestMetricsSettings settings;
    settings.monteCarloSimulationCount = 20;
    const std::map<TradeID, Trade> trades{{1, closed}, {2, open}};
    const auto metrics = calculateBacktestMetrics(
        "fixture", trades, {{100, 100}, {108, 108}, {108, 113}}, market, 100, settings
    );
    require(metrics.tradeCount == 1, "Open campaign counted as a closed trade");
    require(metrics.finalEquity == 113 && metrics.netProfit == 13, "Marked equity lost open PnL");
    require(metrics.grossProfit == 8, "Commission deducted twice");
    require(metrics.averageHoldingBars == 2, "Holding duration used dates or missing legacy counter");
    require(metrics.exposurePercent == 100, "Open campaign omitted from exposure");
    require(metrics.monteCarloTradesPerSimulation == 1, "Unrealized PnL entered Monte Carlo");
    MarketData missingAssetBar = market;
    missingAssetBar[20200102]["ETH"] = BarData{};
    const auto gap = calculateBacktestMetrics(
        "gap", trades, {{100, 100}, {100, 100}, {108, 108}, {108, 113}},
        missingAssetBar, 100, settings
    );
    require(gap.averageHoldingBars == 2, "Another asset's bar extended holding duration through a data gap");
    open.start_ = 20200101;
    const auto openOnly = calculateBacktestMetrics(
        "open", {{2, open}}, {{100, 100}, {100, 105}, {100, 110}}, market, 100, settings
    );
    require(openOnly.tradeCount == 0 && openOnly.exposurePercent == 100,
            "Zero-closed-trade study omitted held exposure");
    require(openOnly.monteCarloSimulationCount == 0, "Open-only campaign bootstrapped");
    settings.tradePnlAlreadyNetOfCommission = false;
    closed.pnl_ = 10;
    const auto gross = calculateBacktestMetrics(
        "gross", {{1, closed}}, {{100, 100}, {108, 108}, {108, 108}}, market, 100, settings
    );
    require(gross.grossProfit == 8, "Gross reporting contract lost fee deduction");
    OHLCVData prices;
    prices.data["BTC"] = {
        {20200101, {100, 101, 99, 100, 1000}},
        {20200102, {100, 101, 99, 100, 1000}},
        {20200103, {100, 106, 99, 105, 1000}},
        {20200104, {110, 115, 85, 90, 1000}},
        {20200105, {80, 85, 75, 80, 1000}}
    };
    StrategyPortfolio portfolio;
    portfolio.emplace_back(
        1, std::make_unique<DonchianSignalStrategy>(1, std::make_unique<AllUniverseSelector>(),
             std::make_unique<AlphabeticalRanker>(), 1, 2, true, 2, "BTC"),
        1.0, std::make_unique<EqualWeightSizer>(0.1), RiskConstraints(1.5, 1.5),
        std::make_unique<EntryExitOnlyRebalancePolicy>()
    );
    BacktestContext context(prices, std::move(portfolio), 100);
    Backtester tester(context);
    tester.loop();
    const auto campaigns = context.GetTradeRecorder().closedTrades();
    require(campaigns.size() == 1, "Donchian duplicated or missed a campaign");
    const auto& campaign = campaigns.front();
    require(campaign.start == 20200104 && campaign.end == 20200105,
            "Donchian filled at signal close instead of next open");
    require(campaign.entry_price == 110 && campaign.exit_price == 80,
            "Donchian fills used close or intrabar prices");
    require(std::abs(campaign.peak_quantity - 10.0 / 110.0) < 1e-12,
            "Donchian quantity did not resolve monetary target at next open");
    require(context.GetAccountHistory()[2].equity == 100,
            "Donchian entry affected the signal-day account");
    require(std::abs(context.GetCurrentEquity() - (100 - 30 * 10.0 / 110.0)) < 1e-12,
            "Donchian realized PnL disagrees with actual fills");
    std::cout << "RESEARCH-REPORT: PASS: open cutoff, fees, observed holding bars and Monte Carlo\n";
}
