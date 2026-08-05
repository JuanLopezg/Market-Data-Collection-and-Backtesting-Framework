#include "backtest_helpers.h"

#include "logger.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
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
    const std::string& coin,
    const std::string& outputHtml
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
    const std::string& outputHtml
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

static std::string scaleCsvTrim(std::string s)
{
    auto notSpace = [](unsigned char c) {
        return !std::isspace(c);
    };

    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());

    return s;
}

static std::string scaleCsvLower(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        }
    );

    return value;
}

static std::vector<std::string> scaleCsvParseLine(const std::string& line)
{
    std::vector<std::string> fields;
    std::string current;
    bool inQuotes = false;

    for (std::size_t i = 0; i < line.size(); ++i)
    {
        char c = line[i];

        if (c == '"')
        {
            if (inQuotes && i + 1 < line.size() && line[i + 1] == '"')
            {
                current += '"';
                ++i;
            }
            else
            {
                inQuotes = !inQuotes;
            }
        }
        else if (c == ',' && !inQuotes)
        {
            fields.push_back(current);
            current.clear();
        }
        else
        {
            current += c;
        }
    }

    fields.push_back(current);
    return fields;
}

static std::string scaleCsvEscape(const std::string& value)
{
    bool needsQuotes = false;

    for (char c : value)
    {
        if (c == ',' || c == '"' || c == '\n' || c == '\r')
        {
            needsQuotes = true;
            break;
        }
    }

    if (!needsQuotes)
        return value;

    std::string escaped;
    escaped.push_back('"');

    for (char c : value)
    {
        if (c == '"')
            escaped += "\"\"";
        else
            escaped.push_back(c);
    }

    escaped.push_back('"');
    return escaped;
}

static void scaleCsvWriteLine(
    std::ofstream& output,
    const std::vector<std::string>& fields
)
{
    for (std::size_t i = 0; i < fields.size(); ++i)
    {
        if (i > 0)
            output << ",";

        output << scaleCsvEscape(fields[i]);
    }

    output << "\n";
}

static int scaleCsvFindColumn(
    const std::vector<std::string>& header,
    const std::vector<std::string>& possibleNames
)
{
    for (std::size_t i = 0; i < header.size(); ++i)
    {
        const std::string columnName =
            scaleCsvLower(scaleCsvTrim(header[i]));

        for (const std::string& possibleName : possibleNames)
        {
            if (columnName == scaleCsvLower(possibleName))
                return static_cast<int>(i);
        }
    }

    return -1;
}

static std::string scaleCsvUpper(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        }
    );

    return value;
}

static std::string scaleCsvNumberString(double value)
{
    std::ostringstream output;
    output << std::fixed << std::setprecision(10) << value;
    return output.str();
}

void scaleLuncOHLCBy1000(
    const std::filesystem::path& input_path,
    const std::filesystem::path& output_path
)
{
    std::ifstream input(input_path);
    if (!input.is_open())
    {
        LG_ERROR("Failed to open input CSV: {}", input_path.string());
        return;
    }

    std::ofstream output(output_path);
    if (!output.is_open())
    {
        LG_ERROR("Failed to open output CSV: {}", output_path.string());
        return;
    }

    std::string line;

    if (!std::getline(input, line))
    {
        LG_ERROR("Input CSV is empty: {}", input_path.string());
        return;
    }

    if (!line.empty() && line.back() == '\r')
        line.pop_back();

    std::vector<std::string> header = scaleCsvParseLine(line);

    const int coinCol = scaleCsvFindColumn(header, {"coin", "symbol", "ticker"});
    const int openCol = scaleCsvFindColumn(header, {"open", "o"});
    const int highCol = scaleCsvFindColumn(header, {"high", "h"});
    const int lowCol = scaleCsvFindColumn(header, {"low", "l"});
    const int closeCol = scaleCsvFindColumn(header, {"close", "c"});

    if (
        coinCol < 0 ||
        openCol < 0 ||
        highCol < 0 ||
        lowCol < 0 ||
        closeCol < 0
    )
    {
        LG_ERROR(
            "Could not find required columns. Need coin/symbol/ticker plus open/high/low/close."
        );
        return;
    }

    scaleCsvWriteLine(output, header);

    std::size_t changedRows = 0;
    std::size_t totalRows = 0;

    while (std::getline(input, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        if (scaleCsvTrim(line).empty())
            continue;

        std::vector<std::string> fields = scaleCsvParseLine(line);

        ++totalRows;

        const std::size_t minRequiredSize =
            static_cast<std::size_t>(
                std::max({coinCol, openCol, highCol, lowCol, closeCol})
            ) + 1;

        if (fields.size() < minRequiredSize)
        {
            LG_ERROR("Skipping malformed CSV row: {}", line);
            continue;
        }

        const std::string coin =
            scaleCsvUpper(scaleCsvTrim(fields[coinCol]));

        if (coin == "LUNC")
        {
            try
            {
                fields[openCol] =
                    scaleCsvNumberString(std::stod(scaleCsvTrim(fields[openCol])) * 1000.0);

                fields[highCol] =
                    scaleCsvNumberString(std::stod(scaleCsvTrim(fields[highCol])) * 1000.0);

                fields[lowCol] =
                    scaleCsvNumberString(std::stod(scaleCsvTrim(fields[lowCol])) * 1000.0);

                fields[closeCol] =
                    scaleCsvNumberString(std::stod(scaleCsvTrim(fields[closeCol])) * 1000.0);

                ++changedRows;
            }
            catch (const std::exception& e)
            {
                LG_ERROR("Failed to scale LUNC row: {}. Error: {}", line, e.what());
            }
        }

        scaleCsvWriteLine(output, fields);
    }

    LG_INFO(
        "Scaled LUNC OHLC by 1000. Changed rows: {} / {}. Output saved to: {}",
        changedRows,
        totalRows,
        output_path.string()
    );
}

void filterCSVByDateRange(
    const std::filesystem::path& input_path,
    const std::filesystem::path& output_path,
    const std::string& start_date,
    const std::string& end_date_exclusive
)
{
    std::ifstream input(input_path);
    if (!input.is_open())
    {
        LG_ERROR("Failed to open input CSV: {}", input_path.string());
        return;
    }

    std::ofstream output(output_path);
    if (!output.is_open())
    {
        LG_ERROR("Failed to open output CSV: {}", output_path.string());
        return;
    }

    std::string line;

    // Copy header
    if (std::getline(input, line))
    {
        output << line << '\n';
    }

    while (std::getline(input, line))
    {
        if (line.empty())
            continue;

        std::stringstream ss(line);

        std::string dateStr;
        std::getline(ss, dateStr, ',');

        if (dateStr.empty())
            continue;

        // Works for YYYY-MM-DD format
        if (dateStr >= start_date && dateStr < end_date_exclusive)
        {
            output << line << '\n';
        }
    }

    LG_INFO("Filtered CSV saved to: {}", output_path.string());
}

