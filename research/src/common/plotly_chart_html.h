#pragma once

#include "data_types.h"

#include <string>
#include <vector>

// Reads and patches the Plotly HTML that the balance/equity chart writer generates:
// finds a named trace's x/y arrays and injects a buy-and-hold benchmark trace.
// Scenario reports use it to reuse each run's chart; nothing here touches trading.
namespace scenario_reports {

struct PlotlyXYExpressions {
    std::string x;
    std::string y;
};

struct BenchmarkPoint {
    std::string timestamp;
    double close;
};

std::string escapeHtmlAttribute(const std::string& value);
// Escapes text for a double-quoted JavaScript literal (quotes not added; <, >, & as \u).
std::string escapeJavaScriptString(const std::string& value);
// YYYYMMDD[HH[MM[SS]]] or Unix seconds/ms as ISO date text; other text passes through.
std::string normalizeTimestampForPlotly(const std::string& rawTimestamp);

// Close of symbol for every bar that has a valid one; throws when there is none.
std::vector<BenchmarkPoint> makeBenchmarkPoints(const MarketData& market, const std::string& symbol);
// The x and y array literals of the trace named trace, resolved through variables.
PlotlyXYExpressions extractNamedPlotlyXYExpressions(const std::string& html, const std::string& trace);
// Adds a buy-and-hold trace that starts at initialBalance on the first benchmark close.
std::string injectBuyAndHoldTraceIntoPlotlyData(std::string html,
    const std::vector<BenchmarkPoint>& points, const std::string& symbol, double initialBalance);

} // namespace scenario_reports
