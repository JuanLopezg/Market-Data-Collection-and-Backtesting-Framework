#pragma once

#include "data_types.h"
#include <filesystem>
#include <string>
#include <vector>

// Scenario reporting owns chart embedding and correlation presentation, not trading.
namespace scenario_reports {

struct ChartEntry {
    std::string datasetId;
    std::string datasetFileName;
    std::string benchmarkSymbol;
    std::string chartHtml;
};

struct CorrelationChartEntry {
    std::string strategyName;

    // Store the resolved arrays themselves, not the Plotly trace object.
    // The generated chart often uses:
    //
    //   const dates = [...];
    //   const equityValues = [...];
    //   const equity = {x: dates, y: equityValues, name: "Equity"};
    //
    // Saving only the object leaves identifiers such as dates undefined in
    // the separate correlation report. These two expressions are resolved
    // to their actual array literals while the original HTML is available.
    std::string datesArrayExpression;
    std::string equityArrayExpression;
};

struct PlotlyXYExpressions {
    std::string x;
    std::string y;
};

struct BenchmarkPoint {
    std::string timestamp;
    double close;
};

bool ensureDirectoryExists(const std::filesystem::path& path);
std::string readTextFile(const std::filesystem::path& path);
std::vector<BenchmarkPoint> makeBenchmarkPoints(const MarketData& market, const std::string& symbol);
PlotlyXYExpressions extractNamedPlotlyXYExpressions(const std::string& html, const std::string& trace);
std::string injectBuyAndHoldTraceIntoPlotlyData(std::string html,
    const std::vector<BenchmarkPoint>& points, const std::string& symbol, double initialBalance);
void writeStrategyIndexHtml(const std::filesystem::path& path, const std::string& strategy,
    const std::vector<ChartEntry>& charts);
void write1dCmcCorrelationHtml(const std::filesystem::path& path,
    const std::vector<CorrelationChartEntry>& strategies, const std::vector<BenchmarkPoint>& benchmark,
    const std::string& symbol, double initialBalance);

} // namespace scenario_reports
