#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "database_utils.h"
#include "fill.h"
#include "price_snapshot.h"
#include "realtest.h"
#include "trade_recorder.h"

namespace {

struct Options {
    std::filesystem::path fills_csv;
    std::filesystem::path realtest_csv;
    std::filesystem::path historical_data;
    std::filesystem::path comparison_csv;
    std::string portfolio_mode = "equal-weight";
};

Options parseOptions(int argc, char** argv)
{
    Options out;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](const char* name) -> std::string {
            if (i + 1 >= argc)
                throw std::invalid_argument(std::string(name) + " requires a value");
            return argv[++i];
        };

        if (arg == "--fills-csv") out.fills_csv = value("--fills-csv");
        else if (arg == "--realtest-csv") out.realtest_csv = value("--realtest-csv");
        else if (arg == "--historical-data") out.historical_data = value("--historical-data");
        else if (arg == "--comparison-csv") out.comparison_csv = value("--comparison-csv");
        else if (arg == "--portfolio-mode") out.portfolio_mode = value("--portfolio-mode");
        else if (arg == "--help" || arg == "-h") {
            std::cout
                << "Usage: t19_realtest_compare --fills-csv PATH --realtest-csv PATH "
                   "--historical-data PATH --comparison-csv PATH "
                   "[--portfolio-mode equal-weight|vol-target]\n";
            std::exit(0);
        }
        else {
            throw std::invalid_argument("Unknown option: " + arg);
        }
    }

    if (out.fills_csv.empty() || out.realtest_csv.empty() || out.historical_data.empty() || out.comparison_csv.empty())
        throw std::invalid_argument("fills, RealTest, historical-data and comparison paths are required");
    if (out.portfolio_mode != "equal-weight" && out.portfolio_mode != "vol-target")
        throw std::invalid_argument("--portfolio-mode must be equal-weight or vol-target");
    return out;
}

std::vector<std::string> splitCsvLine(const std::string& line)
{
    std::vector<std::string> fields;
    std::string current;
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '"') {
            if (quoted && i + 1 < line.size() && line[i + 1] == '"') {
                current.push_back('"');
                ++i;
            }
            else {
                quoted = !quoted;
            }
        }
        else if (c == ',' && !quoted) {
            fields.push_back(current);
            current.clear();
        }
        else {
            current.push_back(c);
        }
    }
    fields.push_back(current);
    return fields;
}

std::vector<Fill> loadFills(const std::filesystem::path& path)
{
    std::ifstream in(path);
    if (!in.is_open())
        throw std::runtime_error("Could not open fills CSV: " + path.string());

    std::string line;
    if (!std::getline(in, line))
        throw std::runtime_error("Fills CSV is empty");
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    if (line != "fill_id,order_id,strategy_id,timestamp,coin,side,quantity,price,commission")
        throw std::runtime_error("Unexpected fills CSV header: " + line);

    std::vector<Fill> fills;
    std::size_t lineNumber = 1;
    while (std::getline(in, line)) {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        const auto f = splitCsvLine(line);
        if (f.size() != 9)
            throw std::runtime_error("Invalid fills CSV field count at line " + std::to_string(lineNumber));

        Fill fill;
        fill.fill_id = static_cast<FillID>(std::stoull(f[0]));
        fill.order_id = static_cast<OrderID>(std::stoull(f[1]));
        fill.strategy_id = static_cast<StrategyID>(std::stoull(f[2]));
        fill.timestamp = static_cast<Timestamp>(std::stoull(f[3]));
        fill.coin = f[4];
        const int side = std::stoi(f[5]);
        if (side == 0) fill.side = OrderSide::Buy;
        else if (side == 1) fill.side = OrderSide::Sell;
        else throw std::runtime_error("Invalid order side at line " + std::to_string(lineNumber));
        fill.quantity = std::stod(f[6]);
        fill.price = std::stod(f[7]);
        fill.commission = std::stod(f[8]);
        fill.validate();
        fills.push_back(std::move(fill));
    }

    std::sort(fills.begin(), fills.end(), [](const Fill& a, const Fill& b) {
        if (a.timestamp != b.timestamp) return a.timestamp < b.timestamp;
        return a.fill_id < b.fill_id;
    });
    return fills;
}

std::pair<Timestamp, PriceSnapshot> finalMarks(const std::filesystem::path& historicalData)
{
    const OHLCVData raw = loadDatabase(historicalData, 0);
    if (raw.data.empty())
        throw std::runtime_error("Historical data could not be loaded for final marks");

    Timestamp finalTimestamp = 0;
    for (const auto& [coin, series] : raw.data) {
        (void)coin;
        if (!series.empty())
            finalTimestamp = std::max(finalTimestamp, series.rbegin()->first);
    }
    if (finalTimestamp == 0)
        throw std::runtime_error("Historical data has no timestamp");

    PriceSnapshot marks;
    for (const auto& [coin, series] : raw.data) {
        const auto it = series.find(finalTimestamp);
        if (it != series.end() && std::isfinite(it->second.close) && it->second.close > 0.0)
            marks.set(coin, it->second.close);
    }
    return {finalTimestamp, marks};
}

std::map<TradeID, Trade> toLegacyTrades(
    const TradeRecorder& recorder,
    const PriceSnapshot& marks,
    Timestamp timestamp
)
{
    std::map<TradeID, Trade> result;
    for (const TradeRecord& record : recorder.allTrades(marks, timestamp)) {
        Trade trade;
        trade.trade_id_ = record.trade_id;
        trade.start_ = record.start;
        trade.end_ = record.end;
        trade.commission_ = record.commission;
        trade.coin_ = record.coin;
        trade.direction_ = record.direction;
        trade.current_price_ = record.exit_price;
        trade.entry_ = record.entry_price;
        trade.exit_ = record.exit_price;
        trade.size_ = record.peak_quantity;
        trade.pnl_ = record.pnl;
        trade.isSimulated_ = false;
        trade.exited_ = record.exited;
        trade.strategy_name_ = record.strategy_name;
        result[trade.trade_id_] = std::move(trade);
    }
    return result;
}

PortfolioSizerKind sizingKind(const std::string& mode)
{
    return mode == "equal-weight" ? PortfolioSizerKind::EqualWeight : PortfolioSizerKind::VolatilityTarget;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const Options options = parseOptions(argc, argv);
        if (!std::filesystem::exists(options.realtest_csv))
            throw std::runtime_error("RealTest CSV not found: " + options.realtest_csv.string());
        if (!std::filesystem::exists(options.historical_data))
            throw std::runtime_error("Historical data not found: " + options.historical_data.string());

        const std::vector<Fill> fills = loadFills(options.fills_csv);
        TradeRecorder recorder;
        for (const Fill& fill : fills) {
            if (fill.strategy_id != 1)
                throw std::runtime_error("Unexpected strategy_id in T19 fills: " + std::to_string(fill.strategy_id));
            recorder.onFill(fill, "Pure_RSI");
        }

        const auto [finalTimestamp, marks] = finalMarks(options.historical_data);
        std::map<TradeID, Trade> trades = toLegacyTrades(recorder, marks, finalTimestamp);
        std::filesystem::path realtestPath = options.realtest_csv;

        std::cout << "============================================================\n";
        std::cout << "T19 DISTRIBUTED FILLS -> EXACT RESEARCH REALTEST POLICY\n";
        std::cout << "============================================================\n";
        std::cout << "Portfolio mode                    : " << options.portfolio_mode << "\n";
        std::cout << "Persisted distributed fills       : " << fills.size() << "\n";
        std::cout << "Distributed trades for RealTest   : " << trades.size() << "\n";
        std::cout << "Final analytical mark timestamp   : " << finalTimestamp << "\n";
        std::cout << "RealTest CSV                       : " << realtestPath.string() << "\n";

        const bool exactResult = compareBacktestBySizing(
            sizingKind(options.portfolio_mode),
            realtestPath,
            trades,
            options.comparison_csv
        );

        // Known accepted discrepancies are validated separately against the locked
        // historical baseline.  Do not reinterpret or loosen research's policy here.
        std::cout << "T19_REALTEST_RESEARCH_POLICY: "
                  << (exactResult ? "MATCH" : "REVIEW_REQUIRED") << "\n";
        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "T19_REALTEST_BRIDGE_ERROR: " << e.what() << "\n";
        return 1;
    }
}
