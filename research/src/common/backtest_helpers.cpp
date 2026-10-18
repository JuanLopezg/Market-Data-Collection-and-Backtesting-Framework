#include "backtest_helpers.h"

#include "logger.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
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



static bool rollingEquityVolatility(
    const std::vector<std::pair<Balance, Equity>>& eqbal,
    std::size_t endIndex,
    std::size_t rollingWindow,
    double periodsPerYear,
    double& result
)
{
    if (rollingWindow < 2 || endIndex < rollingWindow || periodsPerYear <= 0.0)
        return false;

    std::vector<double> returns;
    returns.reserve(rollingWindow);

    const std::size_t firstIndex = endIndex - rollingWindow + 1;
    for (std::size_t i = firstIndex; i <= endIndex; ++i) {
        const double previousEquity = eqbal[i - 1].second;
        const double currentEquity = eqbal[i].second;

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


void printBalanceEquityChart(
    const std::vector<std::pair<Balance, Equity>>& eqbal,
    const MarketData& marketData,
    const std::vector<VolatilityDiagnosticSnapshot>& volatilityDiagnostics,
    std::size_t rollingVolatilityWindow,
    double periodsPerYear,
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

    std::map<Timestamp, const VolatilityDiagnosticSnapshot*> diagnosticsByTimestamp;
    StrategyID plottedStrategyId = 0;
    std::string plottedStrategyName;
    double configuredTargetVolatility = 0.0;

    if (!volatilityDiagnostics.empty()) {
        plottedStrategyId = volatilityDiagnostics.front().strategy_id;
        plottedStrategyName = volatilityDiagnostics.front().strategy_name;
        configuredTargetVolatility = volatilityDiagnostics.front().sizing.target_volatility;

        for (const auto& diagnostic : volatilityDiagnostics) {
            if (diagnostic.strategy_id == plottedStrategyId)
                diagnosticsByTimestamp[diagnostic.timestamp] = &diagnostic;
        }
    }

    std::string dates;
    std::string balances;
    std::string equities;
    std::string realizedVolatilities;
    std::string targetVolatilities;
    std::string preConstraintVolatilities;
    std::string postConstraintVolatilities;

    auto it = marketData.begin();
    bool first = true;

    for (std::size_t i = 0; i < eqbal.size() && it != marketData.end(); ++i, ++it) {
        const Timestamp ts = it->first;
        const double balance = eqbal[i].first;
        const double equity = eqbal[i].second;

        if (!first) {
            dates += ",";
            balances += ",";
            equities += ",";
            realizedVolatilities += ",";
            targetVolatilities += ",";
            preConstraintVolatilities += ",";
            postConstraintVolatilities += ",";
        }

        dates += "'" + timestampToISODate(ts) + "'";
        balances += std::to_string(balance);
        equities += std::to_string(equity);

        double realizedVolatility = 0.0;
        if (rollingEquityVolatility(
                eqbal,
                i,
                rollingVolatilityWindow,
                periodsPerYear,
                realizedVolatility
            )) {
            realizedVolatilities += std::to_string(realizedVolatility * 100.0);
        } else {
            realizedVolatilities += "null";
        }

        if (!volatilityDiagnostics.empty())
            targetVolatilities += std::to_string(configuredTargetVolatility * 100.0);
        else
            targetVolatilities += "null";

        const auto diagnosticIt = diagnosticsByTimestamp.find(ts);
        if (diagnosticIt != diagnosticsByTimestamp.end()) {
            const auto& d = diagnosticIt->second->sizing;
            preConstraintVolatilities += std::to_string(d.pre_constraint_volatility * 100.0);
            postConstraintVolatilities += std::to_string(d.post_constraint_volatility * 100.0);
        } else {
            preConstraintVolatilities += "null";
            postConstraintVolatilities += "null";
        }

        first = false;
    }

    html << R"HTML(
<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <script src="https://cdn.plot.ly/plotly-3.4.0.min.js"></script>
    <title>Backtest Equity and Volatility</title>
</head>
<body>
    <div id="equityChart" style="width:100%;height:55vh;"></div>
    <div id="volatilityChart" style="width:100%;height:40vh;"></div>

    <script>
)HTML";

    html << "const dates = [" << dates << "];\n";
    html << "const balances = [" << balances << "];\n";
    html << "const equities = [" << equities << "];\n";
    html << "const realizedVolatilities = [" << realizedVolatilities << "];\n";
    html << "const targetVolatilities = [" << targetVolatilities << "];\n";
    html << "const preConstraintVolatilities = [" << preConstraintVolatilities << "];\n";
    html << "const postConstraintVolatilities = [" << postConstraintVolatilities << "];\n\n";

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

const equityLayout = {
    title: 'Balance vs Equity',
    xaxis: { title: 'Date', rangeslider: { visible: false } },
    yaxis: { title: 'Value', fixedrange: false },
    dragmode: 'zoom',
    hovermode: 'x unified'
};

const realizedVol = {
    x: dates,
    y: realizedVolatilities,
    type: 'scatter',
    mode: 'lines',
    name: 'Rolling realized vol',
    line: { color: 'blue', width: 2 }
};

const targetVol = {
    x: dates,
    y: targetVolatilities,
    type: 'scatter',
    mode: 'lines',
    name: 'Configured target vol',
    line: { color: 'red', width: 2, dash: 'dash' }
};

const preConstraintVol = {
    x: dates,
    y: preConstraintVolatilities,
    type: 'scatter',
    mode: 'lines',
    name: 'Predicted vol before caps',
    line: { color: 'gray', width: 1 }
};

const postConstraintVol = {
    x: dates,
    y: postConstraintVolatilities,
    type: 'scatter',
    mode: 'lines',
    name: 'Predicted vol after caps',
    line: { color: 'orange', width: 2 }
};

const volatilityLayout = {
    title: 'Volatility Target Validation',
    xaxis: { title: 'Date', rangeslider: { visible: true } },
    yaxis: { title: 'Annualized volatility (%)', fixedrange: false, rangemode: 'tozero' },
    dragmode: 'zoom',
    hovermode: 'x unified'
};

const config = {
    responsive: true,
    scrollZoom: true,
    displaylogo: false
};

Plotly.newPlot('equityChart', [equity, balance], equityLayout, config);
Plotly.newPlot(
    'volatilityChart',
    [realizedVol, targetVol, preConstraintVol, postConstraintVol],
    volatilityLayout,
    config
);
    </script>
</body>
</html>
)HTML";

    html.close();

    LG_INFO("Volatility chart uses {}-bar rolling equity returns, annualized with {} periods per year (strategy {} {})",
            rollingVolatilityWindow, periodsPerYear, plottedStrategyId, plottedStrategyName);

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




void storeBalanceEquityCSV(
    const std::vector<std::pair<Balance, Equity>>& eqbal,
    const MarketData& marketData,
    const std::filesystem::path& outputCsv
)
{
    std::ofstream out(outputCsv);
    if (!out)
        throw std::runtime_error("Cannot write balance/equity CSV: " + outputCsv.string());
    out << "date,balance,equity\n" << std::setprecision(15);

    // eqbal holds one entry per market bar, in market-data order.
    auto it = marketData.begin();
    for (std::size_t i = 0; i < eqbal.size() && it != marketData.end(); ++i, ++it)
        out << timestampToISODate(it->first) << ',' << eqbal[i].first << ',' << eqbal[i].second << '\n';
    LG_INFO("Balance/equity series written: {} rows to {}", eqbal.size(), outputCsv.string());
}


