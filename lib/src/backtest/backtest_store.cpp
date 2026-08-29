#include "backtest.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>


static std::string csvEscape(const std::string& value)
{
    const bool needsQuotes = value.find_first_of(",\"\n\r") != std::string::npos;
    if (!needsQuotes)
        return value;

    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');

    for (char c : value) {
        if (c == '"')
            escaped += "\"\"";
        else
            escaped.push_back(c);
    }

    escaped.push_back('"');
    return escaped;
}


/**************************************************************************************
 * Purpose : Persist analytics TradeRecord objects to SQLite
 **************************************************************************************/
void Backtester::storeTrades(sqlite3* db)
{
    const char* createTableSQL = R"(
        CREATE TABLE IF NOT EXISTS trades (
            trade_id       INTEGER PRIMARY KEY,
            strategy_id    INTEGER NOT NULL,
            strategy_name  TEXT NOT NULL,
            start_ts       INTEGER NOT NULL,
            end_ts         INTEGER NOT NULL,
            commission     REAL NOT NULL,
            coin           TEXT NOT NULL,
            direction      INTEGER NOT NULL,
            entry_price    REAL NOT NULL,
            exit_price     REAL NOT NULL,
            peak_quantity  REAL NOT NULL,
            fill_count     INTEGER NOT NULL,
            exited         INTEGER NOT NULL,
            pnl            REAL NOT NULL
        );
    )";

    if (sqlite3_exec(db, createTableSQL, nullptr, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error("Failed to create trades table");

    const char* insertSQL = R"(
        INSERT INTO trades (
            trade_id, strategy_id, strategy_name, start_ts, end_ts, commission,
            coin, direction, entry_price, exit_price, peak_quantity,
            fill_count, exited, pnl
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(trade_id) DO UPDATE SET
            strategy_id   = excluded.strategy_id,
            strategy_name = excluded.strategy_name,
            start_ts      = excluded.start_ts,
            end_ts        = excluded.end_ts,
            commission    = excluded.commission,
            coin          = excluded.coin,
            direction     = excluded.direction,
            entry_price   = excluded.entry_price,
            exit_price    = excluded.exit_price,
            peak_quantity = excluded.peak_quantity,
            fill_count    = excluded.fill_count,
            exited        = excluded.exited,
            pnl           = excluded.pnl;
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, insertSQL, -1, &stmt, nullptr) != SQLITE_OK)
        throw std::runtime_error("Failed to prepare trades insert statement");

    const auto trades = backtest_context_.GetTradeRecorder().allTrades(
        backtest_context_.GetLastMarks(),
        backtest_context_.GetLastTimestamp()
    );

    if (sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        throw std::runtime_error("Failed to begin transaction for trades");
    }

    try {
        for (const TradeRecord& trade : trades) {
            sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(trade.trade_id));
            sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(trade.strategy_id));
            sqlite3_bind_text(stmt, 3, trade.strategy_name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(trade.start));
            sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(trade.end));
            sqlite3_bind_double(stmt, 6, trade.commission);
            sqlite3_bind_text(stmt, 7, trade.coin.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 8, static_cast<int>(trade.direction));
            sqlite3_bind_double(stmt, 9, trade.entry_price);
            sqlite3_bind_double(stmt, 10, trade.exit_price);
            sqlite3_bind_double(stmt, 11, trade.peak_quantity);
            sqlite3_bind_int(stmt, 12, static_cast<int>(trade.fill_count));
            sqlite3_bind_int(stmt, 13, trade.exited ? 1 : 0);
            sqlite3_bind_double(stmt, 14, trade.pnl);

            if (sqlite3_step(stmt) != SQLITE_DONE)
                throw std::runtime_error("Failed to insert trade row");

            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
        }

        if (sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error("Failed to commit transaction for trades");
    }
    catch (...) {
        sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
        sqlite3_finalize(stmt);
        throw;
    }

    sqlite3_finalize(stmt);
}


/**************************************************************************************
 * Purpose : Persist timestamped realized balance/equity history to SQLite
 **************************************************************************************/
void Backtester::storeBalanceEquity(sqlite3* db)
{
    const char* createTableSQL = R"(
        CREATE TABLE IF NOT EXISTS balance_equity (
            ts      INTEGER PRIMARY KEY,
            balance REAL NOT NULL,
            equity  REAL NOT NULL
        );
    )";

    if (sqlite3_exec(db, createTableSQL, nullptr, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error("Failed to create balance_equity table");

    const char* insertSQL =
        "INSERT OR REPLACE INTO balance_equity (ts, balance, equity) VALUES (?, ?, ?);";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, insertSQL, -1, &stmt, nullptr) != SQLITE_OK)
        throw std::runtime_error("Failed to prepare balance_equity insert statement");

    if (sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        throw std::runtime_error("Failed to begin transaction for balance_equity");
    }

    try {
        for (const AccountSnapshot& snapshot : backtest_context_.GetAccountHistory()) {
            sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(snapshot.timestamp));
            sqlite3_bind_double(stmt, 2, snapshot.balance);
            sqlite3_bind_double(stmt, 3, snapshot.equity);

            if (sqlite3_step(stmt) != SQLITE_DONE)
                throw std::runtime_error("Failed to insert balance_equity row");

            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
        }

        if (sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error("Failed to commit transaction for balance_equity");
    }
    catch (...) {
        sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
        sqlite3_finalize(stmt);
        throw;
    }

    sqlite3_finalize(stmt);
}


void Backtester::storeResults(std::filesystem::path& backtestStorePath)
{
    if (!backtestStorePath.parent_path().empty())
        std::filesystem::create_directories(backtestStorePath.parent_path());

    sqlite3* db = nullptr;
    if (sqlite3_open(backtestStorePath.c_str(), &db) != SQLITE_OK)
        throw std::runtime_error("Failed to create SQLite database");

    try {
        storeBalanceEquity(db);
        storeTrades(db);
    }
    catch (...) {
        sqlite3_close(db);
        throw;
    }

    sqlite3_close(db);
}


void Backtester::storeTradesCSV(const std::filesystem::path& csvStorePath)
{
    if (!csvStorePath.parent_path().empty())
        std::filesystem::create_directories(csvStorePath.parent_path());

    std::ofstream file(csvStorePath);
    if (!file.is_open())
        throw std::runtime_error("Failed to create trades CSV file");

    file << std::setprecision(17);
    file
        << "trade_id,strategy_id,strategy_name,start_ts,end_ts,commission,coin,direction,"
        << "entry_price,exit_price,peak_quantity,fill_count,exited,pnl\n";

    const auto trades = backtest_context_.GetTradeRecorder().allTrades(
        backtest_context_.GetLastMarks(),
        backtest_context_.GetLastTimestamp()
    );

    for (const TradeRecord& trade : trades) {
        file
            << trade.trade_id << ','
            << trade.strategy_id << ','
            << csvEscape(trade.strategy_name) << ','
            << trade.start << ','
            << trade.end << ','
            << trade.commission << ','
            << csvEscape(trade.coin) << ','
            << static_cast<int>(trade.direction) << ','
            << trade.entry_price << ','
            << trade.exit_price << ','
            << trade.peak_quantity << ','
            << trade.fill_count << ','
            << (trade.exited ? 1 : 0) << ','
            << trade.pnl
            << '\n';
    }

    if (!file.good())
        throw std::runtime_error("Failed while writing trades CSV file");
}

static bool rollingAnnualizedVolatility(
    const std::vector<AccountSnapshot>& history,
    Timestamp timestamp,
    std::size_t rollingWindow,
    double periodsPerYear,
    double& result
)
{
    if (rollingWindow < 2 || !std::isfinite(periodsPerYear) || periodsPerYear <= 0.0)
        throw std::invalid_argument("Invalid rolling volatility configuration");

    std::size_t endIndex = history.size();
    for (std::size_t i = 0; i < history.size(); ++i) {
        if (history[i].timestamp == timestamp) {
            endIndex = i;
            break;
        }
    }

    if (endIndex == history.size() || endIndex < rollingWindow)
        return false;

    std::vector<double> returns;
    returns.reserve(rollingWindow);

    const std::size_t firstIndex = endIndex - rollingWindow + 1;
    for (std::size_t i = firstIndex; i <= endIndex; ++i) {
        const double previousEquity = history[i - 1].equity;
        const double currentEquity = history[i].equity;

        if (!std::isfinite(previousEquity) || !std::isfinite(currentEquity) || previousEquity <= 0.0)
            return false;

        returns.push_back(currentEquity / previousEquity - 1.0);
    }

    double mean = 0.0;
    for (double value : returns)
        mean += value;
    mean /= static_cast<double>(returns.size());

    double sumSquared = 0.0;
    for (double value : returns) {
        const double deviation = value - mean;
        sumSquared += deviation * deviation;
    }

    const double variance = sumSquared / static_cast<double>(returns.size() - 1);
    result = std::sqrt(std::max(0.0, variance)) * std::sqrt(periodsPerYear);
    return std::isfinite(result);
}


/**************************************************************************************
 * Purpose : Persist volatility-target validation history to CSV
 *
 * predicted_* values are ex-ante covariance estimates. rolling_realized_volatility is
 * calculated from actual account equity returns and therefore measures what occurred.
 **************************************************************************************/
void Backtester::storeVolatilityDiagnosticsCSV(
    const std::filesystem::path& csvStorePath,
    std::size_t rollingWindow,
    double periodsPerYear
)
{
    if (!csvStorePath.parent_path().empty())
        std::filesystem::create_directories(csvStorePath.parent_path());

    std::ofstream file(csvStorePath);
    if (!file.is_open())
        throw std::runtime_error("Failed to create volatility diagnostics CSV file");

    file << std::setprecision(17);
    file
        << "timestamp,strategy_id,strategy_name,active_assets,target_volatility,"
        << "raw_signal_volatility,scaling_factor,predicted_vol_before_constraints,"
        << "predicted_vol_after_constraints,rolling_realized_volatility,"
        << "gross_before_constraints,gross_after_constraints,max_asset_weight_after_constraints,"
        << "max_gross_leverage_limit,max_asset_weight_limit,asset_cap_binding,gross_cap_binding,"
        << "rebalance_actions\n";

    const auto& accountHistory = backtest_context_.GetAccountHistory();

    std::size_t observations = 0;
    std::size_t unconstrainedObservations = 0;
    std::size_t targetMathMatches = 0;
    std::size_t assetCapDays = 0;
    std::size_t grossCapDays = 0;
    std::size_t realizedVolObservations = 0;
    double realizedVolSum = 0.0;
    double postConstraintVolSum = 0.0;
    double maxObservedGross = 0.0;
    double maxObservedAssetWeight = 0.0;

    for (const VolatilityDiagnosticSnapshot& snapshot : backtest_context_.GetVolatilityDiagnostics()) {
        const auto& d = snapshot.sizing;
        double realizedVolatility = 0.0;
        const bool hasRealizedVolatility = rollingAnnualizedVolatility(
            accountHistory,
            snapshot.timestamp,
            rollingWindow,
            periodsPerYear,
            realizedVolatility
        );

        file
            << snapshot.timestamp << ','
            << snapshot.strategy_id << ','
            << csvEscape(snapshot.strategy_name) << ','
            << d.active_assets << ','
            << d.target_volatility << ','
            << d.raw_signal_volatility << ','
            << d.scaling_factor << ','
            << d.pre_constraint_volatility << ','
            << d.post_constraint_volatility << ',';

        if (hasRealizedVolatility)
            file << realizedVolatility;

        ++observations;
        postConstraintVolSum += d.post_constraint_volatility;
        maxObservedGross = std::max(maxObservedGross, d.gross_after_constraints);
        maxObservedAssetWeight = std::max(
            maxObservedAssetWeight,
            d.max_asset_weight_after_constraints
        );

        if (d.asset_cap_binding)
            ++assetCapDays;
        if (d.gross_cap_binding)
            ++grossCapDays;
        if (!d.asset_cap_binding && !d.gross_cap_binding) {
            ++unconstrainedObservations;
            if (std::abs(d.pre_constraint_volatility - d.target_volatility) <= 1e-10)
                ++targetMathMatches;
        }
        if (hasRealizedVolatility) {
            ++realizedVolObservations;
            realizedVolSum += realizedVolatility;
        }

        file
            << ',' << d.gross_before_constraints
            << ',' << d.gross_after_constraints
            << ',' << d.max_asset_weight_after_constraints
            << ',' << d.max_gross_leverage_limit
            << ',' << d.max_asset_weight_limit
            << ',' << (d.asset_cap_binding ? 1 : 0)
            << ',' << (d.gross_cap_binding ? 1 : 0)
            << ',' << d.rebalance_actions
            << '\n';
    }

    if (!file.good())
        throw std::runtime_error("Failed while writing volatility diagnostics CSV file");

    std::cout << "\n============================================================\n";
    std::cout << "VOLATILITY TARGET VALIDATION SUMMARY\n";
    std::cout << "============================================================\n";
    std::cout << "Diagnostic observations                 : " << observations << "\n";
    std::cout << "Unconstrained observations              : " << unconstrainedObservations << "\n";
    std::cout << "Pre-cap vol matches target              : "
              << targetMathMatches << "/" << unconstrainedObservations << "\n";
    std::cout << "Asset-cap binding observations          : " << assetCapDays << "\n";
    std::cout << "Gross-cap binding observations          : " << grossCapDays << "\n";
    std::cout << "Maximum target gross after constraints  : " << maxObservedGross * 100.0 << "%\n";
    std::cout << "Maximum target asset after constraints  : " << maxObservedAssetWeight * 100.0 << "%\n";

    if (observations > 0)
        std::cout << "Average predicted vol after constraints : "
                  << postConstraintVolSum / static_cast<double>(observations) * 100.0 << "%\n";

    if (realizedVolObservations > 0)
        std::cout << "Average rolling realized volatility     : "
                  << realizedVolSum / static_cast<double>(realizedVolObservations) * 100.0 << "%\n";

    std::cout << "Diagnostics CSV                         : " << csvStorePath.string() << "\n";
}

