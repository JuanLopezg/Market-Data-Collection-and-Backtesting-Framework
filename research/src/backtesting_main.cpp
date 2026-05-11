#include "backtest.h"
#include "bargainChaser.h"
#include "indicator_ranker.h"
#include "indicator_spec.h"
#include "liquidity_universe.h"
#include "universe_selector.h"
#include "database_utils.h"
#include "data_types.h"
#include "csv_utils.h"
#include "logger.h"
#include "realtest.h"

#include <sqlite3.h>

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>


static std::string timestampToISODate(Timestamp ts)
{
    std::string s = std::to_string(ts); // 20200101

    if (s.size() != 8) {
        return s;
    }

    return s.substr(0, 4) + "-" + s.substr(4, 2) + "-" + s.substr(6, 2);
}


void printCoinTradesPlotlyChart(
    const MarketData& marketData,
    const std::map<TradeID, Trade>& tradesHistory,
    const std::string& coin = "AAVE",
    const std::string& outputHtml =
        "/mnt/c/Users/Juan/Documents/Python/algoTrading/storage/backtests/aave_trades.html"
)
{
    std::filesystem::create_directories(
        std::filesystem::path(outputHtml).parent_path()
    );

    std::ofstream html(outputHtml);

    if (!html.is_open()) {
        return;
    }

    std::string dates;
    std::string opens;
    std::string highs;
    std::string lows;
    std::string closes;

    std::string entryDates;
    std::string entryPrices;

    std::string exitDates;
    std::string exitPrices;

    bool first = true;

    for (const auto& [ts, coinMap] : marketData) {
        auto it = coinMap.find(coin);

        if (it == coinMap.end()) {
            continue;
        }

        const BarData& bar = it->second;

        if (!first) {
            dates  += ",";
            opens  += ",";
            highs  += ",";
            lows   += ",";
            closes += ",";
        }

        dates  += "'" + timestampToISODate(ts) + "'";
        opens  += std::to_string(bar.open);
        highs  += std::to_string(bar.high);
        lows   += std::to_string(bar.low);
        closes += std::to_string(bar.close);

        first = false;
    }

    first = true;

    for (const auto& [tradeId, trade] : tradesHistory) {
        (void)tradeId;

        if (trade.coin_ != coin) {
            continue;
        }

        if (trade.start_ == 0) {
            continue;
        }

        if (!first) {
            entryDates  += ",";
            entryPrices += ",";
        }

        entryDates  += "'" + timestampToISODate(trade.start_) + "'";
        entryPrices += std::to_string(trade.entry_);

        first = false;
    }

    first = true;

    for (const auto& [tradeId, trade] : tradesHistory) {
        (void)tradeId;

        if (trade.coin_ != coin) {
            continue;
        }

        if (!trade.exited_ || trade.end_ == 0) {
            continue;
        }

        if (!first) {
            exitDates  += ",";
            exitPrices += ",";
        }

        exitDates  += "'" + timestampToISODate(trade.end_) + "'";
        exitPrices += std::to_string(trade.exit_);

        first = false;
    }

    html << R"HTML(
<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <script src="https://cdn.plot.ly/plotly-3.4.0.min.js"></script>
    <title>Trades Chart</title>
</head>
<body>
    <div id="chart" style="width:100%;height:95vh;"></div>

    <script>
)HTML";

    html << "const candle = {\n";
    html << "  x: [" << dates << "],\n";
    html << "  open: [" << opens << "],\n";
    html << "  high: [" << highs << "],\n";
    html << "  low: [" << lows << "],\n";
    html << "  close: [" << closes << "],\n";
    html << "  type: 'candlestick',\n";
    html << "  name: '" << coin << " OHLC'\n";
    html << "};\n\n";

    html << "const entries = {\n";
    html << "  x: [" << entryDates << "],\n";
    html << "  y: [" << entryPrices << "],\n";
    html << "  mode: 'markers',\n";
    html << "  type: 'scatter',\n";
    html << "  name: 'Entries',\n";
    html << "  marker: { color: 'blue', size: 10, symbol: 'triangle-up' }\n";
    html << "};\n\n";

    html << "const exits = {\n";
    html << "  x: [" << exitDates << "],\n";
    html << "  y: [" << exitPrices << "],\n";
    html << "  mode: 'markers',\n";
    html << "  type: 'scatter',\n";
    html << "  name: 'Exits',\n";
    html << "  marker: { color: 'red', size: 10, symbol: 'x' }\n";
    html << "};\n\n";

    html << R"HTML(
const layout = {
    title: 'Candlestick Chart with Trades',
    xaxis: {
        title: 'Date',
        rangeslider: { visible: true }
    },
    yaxis: {
        title: 'Price',
        fixedrange: false
    },
    dragmode: 'zoom',
    hovermode: 'x unified'
};

const config = {
    responsive: true,
    scrollZoom: true,
    displaylogo: false
};

Plotly.newPlot('chart', [candle, entries, exits], layout, config);
    </script>
</body>
</html>
)HTML";

    html.close();

#ifdef _WIN32
    std::string cmd = "start \"\" \"" + outputHtml + "\"";
#elif __APPLE__
    std::string cmd = "open \"" + outputHtml + "\"";
#else
    std::string cmd = "xdg-open \"" + outputHtml + "\" >/dev/null 2>&1 &";
#endif

    const int result = std::system(cmd.c_str());
    (void)result;
}


void printBalanceEquityChart(
    const std::vector<std::pair<Balance, Equity>>& eqbal,
    const MarketData& marketData,
    const std::string& outputHtml =
        "/mnt/c/Users/Juan/Documents/Python/algoTrading/storage/backtests/balance_equity.html"
)
{
    if (eqbal.empty() || marketData.empty()) {
        return;
    }

    std::filesystem::create_directories(
        std::filesystem::path(outputHtml).parent_path()
    );

    std::ofstream html(outputHtml);

    if (!html.is_open()) {
        return;
    }

    std::string dates;
    std::string balances;
    std::string equities;

    auto it = marketData.begin();
    bool first = true;

    for (std::size_t i = 0; i < eqbal.size() && it != marketData.end(); ++i, ++it) {
        Timestamp ts = it->first;

        double balance = eqbal[i].first;
        double equity  = eqbal[i].second;

        if (!first) {
            dates    += ",";
            balances += ",";
            equities += ",";
        }

        dates    += "'" + timestampToISODate(ts) + "'";
        balances += std::to_string(balance);
        equities += std::to_string(equity);

        first = false;
    }

    html << R"HTML(
<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <script src="https://cdn.plot.ly/plotly-3.4.0.min.js"></script>
    <title>Balance vs Equity</title>
</head>
<body>
    <div id="chart" style="width:100%;height:95vh;"></div>

    <script>
)HTML";

    html << "const dates = [" << dates << "];\n";
    html << "const balances = [" << balances << "];\n";
    html << "const equities = [" << equities << "];\n\n";

    html << R"HTML(
const equity = {
    x: dates,
    y: equities,
    type: 'scatter',
    mode: 'lines',
    name: 'Equity',
    line: { color: 'blue', width: 2 }
};

const balance = {
    x: dates,
    y: balances,
    type: 'scatter',
    mode: 'lines',
    name: 'Balance',
    line: { color: 'red', width: 2 }
};

const layout = {
    title: 'Balance vs Equity',
    xaxis: {
        title: 'Date',
        rangeslider: { visible: true }
    },
    yaxis: {
        title: 'Value',
        fixedrange: false
    },
    dragmode: 'zoom',
    hovermode: 'x unified'
};

const config = {
    responsive: true,
    scrollZoom: true,
    displaylogo: false
};

Plotly.newPlot('chart', [equity, balance], layout, config);
    </script>
</body>
</html>
)HTML";

    html.close();

#ifdef _WIN32
    std::string cmd = "start \"\" \"" + outputHtml + "\"";
#elif __APPLE__
    std::string cmd = "open \"" + outputHtml + "\"";
#else
    std::string cmd = "xdg-open \"" + outputHtml + "\" >/dev/null 2>&1 &";
#endif

    const int result = std::system(cmd.c_str());
    (void)result;
}


void printTradesHistory(const std::map<TradeID, Trade>& tradesHistory)
{
    if (tradesHistory.empty()) {
        LG_INFO("Trades history is empty");
        return;
    }

    LG_INFO("Trades history count={}", tradesHistory.size());

    for (const auto& [tradeId, trade] : tradesHistory) {
        LG_INFO(
            "TRADE id={} coin={} strategy={} direction={} "
            "start={} end={} entry={} exit={} current_price={} "
            "size={} pnl={} commission={} "
            "sl={} slReference={} barsHeld={} "
            "isSimulated={} exited={}",
            tradeId,
            trade.coin_,
            trade.strategy_name_,
            static_cast<int>(trade.direction_),
            trade.start_,
            trade.end_,
            trade.entry_,
            trade.exit_,
            trade.current_price_,
            trade.size_,
            trade.pnl_,
            trade.commission_,
            trade.sl_,
            trade.slReference_,
            trade.barsHeld,
            trade.isSimulated_,
            trade.exited_
        );
    }
}


int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    Logger::Instance().Setup(
        true,   // debug enabled
        false,  // quiet
        "",     // file appender
        "",     // rolling appender
        true    // include header
    );

    std::string path =
        "/mnt/c/Users/Juan/Documents/Python/algoTrading/storage/databases/1d_cmc.csv";

    LG_INFO("Database loading started");
    OHLCVData ohlcvData = loadDatabase(path, 00000000);
    LG_INFO("Database loaded successfully");

    unsigned int lastTradeId = 0;

    double balance = 100000.0;
    double equity = balance;

    double feeTaker = 0.045 / 100.0;
    double feeMaker = 0.015 / 100.0;

    std::vector<std::unique_ptr<Strategy>> strategies;

    // ------------------------------------------------------------------
    // BargainChaser strategy
    // ------------------------------------------------------------------
    unsigned int maxPosOpen = 10;
    double riskPerTrade = 0.1;

    /*
     * Universe selector:
     *
     * Keep only the top 20 most liquid coins at each timestamp.
     * Liquidity is measured as SMA(Volume, 25).
     */
    unsigned int topLiquidityCount = 20;

    std::unique_ptr<UniverseSelector> universeSelector =
        std::make_unique<TopNLiquidityUniverse>(
            IndicatorSpec{
                IndicatorKind::SMA,
                PriceField::Volume,
                25
            },
            topLiquidityCount,
            true
        );

    /*
     * Ranker:
     *
     * Rank only those top 20 liquid coins by ROC(Close, 1), ascending.
     *
     * descending = false means lowest ROC first:
     *   -20% before -10% before +5%
     *
     * So the biggest 1-day losers are ranked first.
     */
    std::unique_ptr<Ranker> ranker =
        std::make_unique<IndicatorRanker>(
            IndicatorSpec{
                IndicatorKind::ROC,
                PriceField::Close,
                1
            },
            false
        );

    double commissionEntryFactor = feeMaker;
    double commissionExitFactor = feeTaker;

    /*
     * Since the universe already contains only the top 20 liquid coins,
     * this scans all 20 after ranking by inverse ROC.
     */
    unsigned int maxRankingPosition = topLiquidityCount;

    unsigned int nBarsExit = 2;
    double fallPercentage = 10.0;
    unsigned int maLength = 50;

    strategies.push_back(
        std::make_unique<StrategyBargainChaser>(
            maxPosOpen,
            riskPerTrade,
            std::move(universeSelector),
            std::move(ranker),
            commissionEntryFactor,
            commissionExitFactor,
            maxRankingPosition,
            nBarsExit,
            fallPercentage,
            maLength
        )
    );

    BacktestContext context(
        ohlcvData,
        strategies,
        balance,
        equity,
        lastTradeId,
        feeMaker,
        feeTaker,
        false
    );

    const MarketData& marketData = context.GetMarketData();

    LG_INFO("Market data and indicators initialized");

    Backtester tester(context);

    LG_INFO("Starting loop");
    tester.loop();
    LG_INFO("Finished loop");

    tester.closeTrades();

    std::filesystem::path backtestStoreDir =
        "/mnt/c/Users/Juan/Documents/Python/algoTrading/storage/backtests/";

    std::filesystem::path backtestStorePath =
        generateBacktestDbPath(backtestStoreDir);

    (void)backtestStorePath;

    // tester.storeResults(backtestStorePath);

    const std::string curveFilename =
        "/mnt/c/Users/Juan/Documents/Python/algoTrading/storage/backtests/balance_equity_curve.csv";

    (void)curveFilename;

    // saveCurveToCSV(context.GetBalanceEquityHistoric(), curveFilename);

    printBalanceEquityChart(
        context.GetBalanceEquityHistoric(),
        marketData,
        "/mnt/c/Users/Juan/Documents/Python/algoTrading/storage/backtests/balance_equity.html"
    );

    // Optional trade chart
    // printCoinTradesPlotlyChart(
    //     marketData,
    //     context.GetTradesHistory(),
    //     "AAVE",
    //     "/mnt/c/Users/Juan/Documents/Python/algoTrading/storage/backtests/aave_trades.html"
    // );

    std::filesystem::path rt =
        "/mnt/c/Users/Juan/Documents/Python/algoTrading/storage/backtests/bargainChaser_rt.csv";

    std::filesystem::path mismatchesCsv =
        "/mnt/c/Users/Juan/Documents/Python/algoTrading/storage/backtests/bargainChaser_mismatches.csv";

    compareBacktests(
        rt,
        context.GetTradesHistory(),
        false,
        mismatchesCsv
    );

    return 0;
}