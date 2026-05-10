#include "backtest.h"
#include "bargainChaser.h"
#include "ranker.h"
#include "database_utils.h"
#include "data_types.h"
#include "csv_utils.h"
#include "logger.h"
#include "realtest.h"

#include <sqlite3.h>
#include <iostream>
#include <map>
#include <string>
#include <fstream>
#include <vector>
#include <utility>

#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <filesystem>



static std::string timestampToISODate(Timestamp ts)
{
    std::string s = std::to_string(ts); // 20200101

    if (s.size() != 8)
        return s;

    return s.substr(0, 4) + "-" + s.substr(4, 2) + "-" + s.substr(6, 2);
}

void printCoinTradesPlotlyChart(
    const EnrichedData& marketData,
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
    if (!html.is_open())
        return;

    std::string dates, opens, highs, lows, closes;
    std::string entryDates, entryPrices;
    std::string exitDates, exitPrices;

    bool first = true;

    for (const auto& [ts, coinMap] : marketData)
    {
        auto it = coinMap.find(coin);
        if (it == coinMap.end())
            continue;

        const BarData& bar = it->second;

        if (!first)
        {
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

    for (const auto& [tradeId, trade] : tradesHistory)
    {
        (void)tradeId;

        if (trade.coin_ != coin)
            continue;

        if (trade.start_ == 0)
            continue;

        if (!first)
        {
            entryDates  += ",";
            entryPrices += ",";
        }

        entryDates  += "'" + timestampToISODate(trade.start_) + "'";
        entryPrices += std::to_string(trade.entry_);

        first = false;
    }

    first = true;

    for (const auto& [tradeId, trade] : tradesHistory)
    {
        (void)tradeId;

        if (trade.coin_ != coin)
            continue;

        if (!trade.exited_ || trade.end_ == 0)
            continue;

        if (!first)
        {
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

    std::system(cmd.c_str());
}


void printBalanceEquityChart(
    const std::vector<std::pair<Balance, Equity>>& eqbal,
    const EnrichedData& marketData,
    const std::string& outputHtml =
        "/mnt/c/Users/Juan/Documents/Python/algoTrading/storage/backtests/balance_equity.html"
)
{
    if (eqbal.empty() || marketData.empty())
        return;

    std::filesystem::create_directories(
        std::filesystem::path(outputHtml).parent_path()
    );

    std::ofstream html(outputHtml);
    if (!html.is_open())
        return;

    std::string dates;
    std::string balances;
    std::string equities;

    auto it = marketData.begin();
    bool first = true;

    for (size_t i = 0; i < eqbal.size() && it != marketData.end(); ++i, ++it)
    {
        Timestamp ts = it->first;

        double balance = eqbal[i].first;
        double equity  = eqbal[i].second;

        if (!first)
        {
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

    std::system(cmd.c_str());
}

void printTradesHistory(const std::map<TradeID, Trade>& tradesHistory)
{
    if (tradesHistory.empty())
    {
        LG_INFO("Trades history is empty");
        return;
    }

    LG_INFO("Trades history count={}", tradesHistory.size());

    for (const auto& [tradeId, trade] : tradesHistory)
    {
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
    // ----------------------------------------------------
    // Minimal logger setup (console only)
    // ----------------------------------------------------
    Logger::Instance().Setup(
        true,   // debug enabled
        false,  // quiet
        "",     // file appender
        "",     // rolling appender
        true    // include header
    );

    /* std::string path = "/mnt/c/Users/Juan/Documents/Python/algoTrading/storage/databases/top_20_database.db";

    LG_INFO("Database loading started");
    OHLCVData ohlcvData = loadDatabase(path, 00000000);
    LG_INFO("Database loaded successful"); */

    std::string path = "/mnt/c/Users/Juan/Documents/Python/algoTrading/storage/databases/1d_cmc.csv";
    OHLCVData ohlcvData = loadDatabase(path, 00000000);

    EnrichedData market_data = buildEnriched(ohlcvData);
    LG_INFO("Enriched data generation finished");

    unsigned int last_trade_id = 0;
    double balance = 100000.0;
    double equity = balance;
    double feeTaker = 0.045 / 100.0; // HL fees
    double feeMaker = 0.015 / 100.0; // HL fees

    /***********************************************************************************************************************/
    std::vector<std::unique_ptr<Strategy>> strategies;

    //-----------------------------------------------------
    // High Breakout
    unsigned int maxPosOpen = 10;
    double riskPerTrade = 0.1;
    std::unique_ptr<Ranker> ranker = std::make_unique<ROCRankerI>(); //std::make_unique<VolumeRanker>();
    double commissionEntryFactor = feeMaker;
    double commissionExitFactor = feeTaker;
    unsigned int maxRankingPosition = 1000000; // there is no limit in the ranking, just that its >10% drop 
    unsigned int nBarsExit = 2;
    double fallPercentage = 10;

    strategies.push_back(
        std::make_unique<StrategyBargainChaser>(
            maxPosOpen,
            riskPerTrade,
            std::move(ranker),
            commissionEntryFactor,
            commissionExitFactor,
            maxRankingPosition,
            nBarsExit,
            fallPercentage
        )
    );

    //-----------------------------------------------------
    // Another strategy

    /***********************************************************************************************************************/


    BacktestContext context = BacktestContext(market_data, strategies, balance, equity, last_trade_id, feeMaker, feeTaker);
    Backtester tester = Backtester(context);
    
    LG_INFO("Starting loop");
    tester.loop();
    LG_INFO("Finished loop");

    tester.closeTrades();
    std::filesystem::path backtest_store_dir = "/mnt/c/Users/Juan/Documents/Python/algoTrading/storage/backtests/";
    std::filesystem::path backtest_store_path = generateBacktestDbPath(backtest_store_dir);

    //tester.storeResults(backtest_store_path);

    const std::string filename = "/mnt/c/Users/Juan/Documents/Python/algoTrading/storage/backtests/balance_equity_curve.csv";
    //saveCurveToCSV(context.GetBalanceEquityHistoric(), filename);


    printBalanceEquityChart(context.GetBalanceEquityHistoric(),market_data,"/mnt/c/Users/Juan/Documents/Python/algoTrading/storage/backtests/balance_equity.html");
    std::filesystem::path rt = "/mnt/c/Users/Juan/Documents/Python/algoTrading/storage/backtests/bargainChaser_rt.csv";
    //compareBacktests(rt,context.GetTradesHistory(), false);
    return 0;
}
