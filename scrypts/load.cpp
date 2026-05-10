#include "backtest.h"
#include "strategy_high_breakout.h"
#include "ranker.h"
#include "database_utils.h"
#include "data_types.h"
#include "csv_utils.h"
#include "logger.h"

#include <sqlite3.h>

#include <fstream>
#include <sstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <utility>
#include <stdexcept>
#include <iomanip> // <-- important for formatting

static std::string trim(const std::string& s)
{
    const std::string whitespace = " \t\r\n";
    const size_t start = s.find_first_not_of(whitespace);
    if (start == std::string::npos) return "";
    const size_t end = s.find_last_not_of(whitespace);
    return s.substr(start, end - start + 1);
}

static std::vector<std::string> loadSymbols(const std::string& symbolsPath)
{
    std::vector<std::string> symbols;
    std::ifstream file(symbolsPath);

    if (!file.is_open()) {
        throw std::runtime_error("Could not open symbols file: " + symbolsPath);
    }

    std::string line;
    while (std::getline(file, line)) {
        std::stringstream ss(line);
        std::string token;

        while (std::getline(ss, token, ',')) {
            token = trim(token);
            if (!token.empty()) {
                symbols.push_back(token);
            }
        }
    }

    return symbols;
}

static std::string yyyymmddToDateString(unsigned int yyyymmdd)
{
    std::string s = std::to_string(yyyymmdd);

    while (s.size() < 8) {
        s = "0" + s;
    }

    return s.substr(0, 4) + "-" + s.substr(4, 2) + "-" + s.substr(6, 2);
}

int main(int argc, char** argv)
{
    Logger::Instance().Setup(true, false, "", "", true);

    try {
        const std::string dbPath =
            "/mnt/c/Users/Juan/Documents/Python/algoTrading/storage/databases/top_20_database.db";

        const std::string symbolsPath =
            "/mnt/c/Users/Juan/Documents/Python/algoTrading/all_symbols.txt";

        const std::string outputCsvPath =
            "/mnt/c/Users/Juan/Documents/Python/algoTrading/ohlcv_fixed.csv";

        LG_INFO("Database loading started");
        OHLCVData ohlcvData = loadDatabase(dbPath, 0);
        LG_INFO("Database loaded successful");

        LG_INFO("Loading symbols file");
        std::vector<std::string> symbols = loadSymbols(symbolsPath);
        LG_INFO("Symbols loaded: " + std::to_string(symbols.size()));

        std::ofstream outFile(outputCsvPath);
        if (!outFile.is_open()) {
            LG_ERROR("Could not open output CSV file: " + outputCsvPath);
            return 1;
        }

        // Force NON-scientific notation
        outFile << std::fixed;

        // CSV header
        outFile << "date,symbol,open,high,low,close,volume\n";

        std::size_t rowsWritten = 0;
        std::size_t symbolsFound = 0;
        std::size_t symbolsMissing = 0;

        for (const std::string& symbol : symbols) {

            auto coinIt = ohlcvData.data.find(symbol);
            if (coinIt == ohlcvData.data.end()) {
                LG_ERROR("Symbol not found in database: " + symbol);
                ++symbolsMissing;
                continue;
            }

            ++symbolsFound;
            const auto& timeSeries = coinIt->second;

            for (const auto& [dateInt, ohlcv] : timeSeries) {

                outFile
                    << yyyymmddToDateString(dateInt) << ","
                    << symbol << ","
                    << std::setprecision(6) << ohlcv.open << ","
                    << std::setprecision(6) << ohlcv.high << ","
                    << std::setprecision(6) << ohlcv.low << ","
                    << std::setprecision(6) << ohlcv.close << ","
                    << std::setprecision(0) << ohlcv.volume << "\n";

                ++rowsWritten;
            }
        }

        outFile.close();

        LG_INFO("CSV export completed: " + outputCsvPath);
        LG_INFO("Symbols found: " + std::to_string(symbolsFound));
        LG_INFO("Symbols missing: " + std::to_string(symbolsMissing));
        LG_INFO("Rows written: " + std::to_string(rowsWritten));
    }
    catch (const std::exception& ex) {
        LG_ERROR(std::string("Exception: ") + ex.what());
        return 1;
    }
    catch (...) {
        LG_ERROR("Unknown exception");
        return 1;
    }

    return 0;
}