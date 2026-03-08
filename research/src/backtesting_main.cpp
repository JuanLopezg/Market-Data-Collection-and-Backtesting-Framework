#include "backtest.h"
#include "strategy_high_breakout.h"
#include "ranker.h"
#include "database_utils.h"

#include <sqlite3.h>
#include <iostream>
#include <map>
#include <string>
#include <fstream>
#include <vector>
#include <string>
#include <utility>
#include <iostream>

using Balance = double;
using Equity  = double;

void saveCurveToCSV(const std::vector<std::pair<Balance, Equity>>& curve)
{
    const std::string filename =
        "/mnt/c/Users/Juan/Documents/Python/algoTrading/storage/backtests/balance_equity_curve.csv";

    std::ofstream file(filename);

    if (!file.is_open())
    {
        std::cerr << "❌ Failed to open file: " << filename << std::endl;
        return;
    }

    file << "Index;Balance;Equity\n";

    for (size_t i = 0; i < curve.size(); ++i)
    {
        file << i << ";"
             << curve[i].first << ";"
             << curve[i].second << "\n";
    }

    file.close();

    std::cout << "✅ Curve saved to: " << filename << std::endl;
}


EnrichedData buildEnriched(const OHLCVData& raw)
{
    EnrichedData enriched;

    for (const auto& [coin, series] : raw.data)
    {
        std::vector<unsigned int> dates;
        dates.reserve(series.size());

        for (const auto& [ts, _] : series)
            dates.push_back(ts);

        std::vector<double> highs, lows, closes, opens, volumes;
        highs.reserve(series.size());
        lows.reserve(series.size());
        closes.reserve(series.size());
        opens.reserve(series.size());
        volumes.reserve(series.size());

        for (const auto& [ts, ohlcv] : series)
        {
            opens.push_back(ohlcv.open);
            highs.push_back(ohlcv.high);
            lows.push_back(ohlcv.low);
            closes.push_back(ohlcv.close);
            volumes.push_back(ohlcv.volume);
        }

        std::vector<double> tr(highs.size(), 0.0);

        // ================= TRUE RANGE =================
        for (size_t i = 1; i < highs.size(); ++i)
        {
            double hl = highs[i] - lows[i];
            double hc = std::abs(highs[i] - closes[i - 1]);
            double lc = std::abs(lows[i]  - closes[i - 1]);

            tr[i] = std::max({hl, hc, lc});
        }

        // ================= BUILD BAR DATA =================
        for (size_t i = 0; i < dates.size(); ++i)
        {
            BarData bar;

            bar.open   = opens[i];
            bar.high   = highs[i];
            bar.low    = lows[i];
            bar.close  = closes[i];
            bar.volume = volumes[i];
            bar.barNumber = static_cast<unsigned int>(i);

            // -------- 20D HIGH (previous 20 bars) --------
            if (i >= 20)
            {
                double maxHigh = highs[i - 20];

                for (size_t j = i - 20; j < i; ++j)
                    maxHigh = std::max(maxHigh, highs[j]);

                bar.high_20d = maxHigh;
            }
            else
            {
                bar.high_20d = 0.0;
            }

            // -------- ATR(14) using previous 14 TR --------
            if (i >= 14)
            {
                double sumTR = 0.0;

                for (size_t j = i - 14; j < i; ++j)
                    sumTR += tr[j];

                bar.atr_14d = sumTR / 14.0;
            }
            else
            {
                bar.atr_14d = 0.0;
            }

            enriched[dates[i]][coin] = bar;
        }
    }

    return enriched;
}

int main(int argc, char** argv)
{
    std::string path =
        "/mnt/c/Users/Juan/Documents/Python/algoTrading/storage/databases/top_20_database.db";

    // ================= OPEN DB =================
    sqlite3* db = nullptr;

    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK)
    {
        std::cerr << "Failed to open DB: " << sqlite3_errmsg(db) << std::endl;
        return 1;
    }


        const char* sql =
    "SELECT pair, date, open, high, low, close, volume "
    "FROM ohlcv_data "
    "WHERE date >= 20170101 "
    "ORDER BY pair, date ASC;";


    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        std::cerr << "Prepare failed: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_close(db);
        return 1;
    }

    OHLCVData ohlcvData;

    // ================= READ ROWS =================
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const char* pair_c =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));

        if (!pair_c) continue;

        std::string pair(pair_c);

        unsigned int date =
            static_cast<unsigned int>(sqlite3_column_int(stmt, 1));

        OHLCV candle;
        candle.open   = sqlite3_column_double(stmt, 2);
        candle.high   = sqlite3_column_double(stmt, 3);
        candle.low    = sqlite3_column_double(stmt, 4);
        candle.close  = sqlite3_column_double(stmt, 5);
        candle.volume = sqlite3_column_double(stmt, 6);

        ohlcvData.data[pair][date] = candle;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    EnrichedData market_data = buildEnriched(ohlcvData);

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
    double riskPerTrade = 0.005;
    std::unique_ptr<Ranker> ranker = std::make_unique<VolumeRanker>();
    double commissionEntryFactor = feeMaker;
    double commissionExitFactor = feeTaker;
    unsigned int maxRankingPosition = 20;

    strategies.push_back(
        std::make_unique<StrategyHighBreakout>(
            maxPosOpen,
            riskPerTrade,
            std::move(ranker),
            commissionEntryFactor,
            commissionExitFactor,
            maxRankingPosition
        )
    );

    //-----------------------------------------------------
    // Another strategy

    /***********************************************************************************************************************/


    BacktestContext context = BacktestContext(market_data, strategies, balance, equity, last_trade_id, feeMaker, feeTaker);
    Backtester tester = Backtester(context);
    
    tester.loop();

    std::filesystem::path backtest_store_dir = "/mnt/c/Users/Juan/Documents/Python/algoTrading/storage/backtests/";
    std::filesystem::path backtest_store_path = generateBacktestDbPath(backtest_store_dir);

    tester.storeResults(backtest_store_path);

    saveCurveToCSV(context.GetBalanceEquityHistoric());

    return 0;
}
