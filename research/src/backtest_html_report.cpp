#include "backtest_html_report.h"

#include "logger.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

struct EquityPoint {
    Timestamp timestamp = 0;
    Equity equity = 0.0;
};

std::string htmlEscape(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size());

    for (const char character : value) {
        switch (character) {
        case '&':  escaped += "&amp;";  break;
        case '<':  escaped += "&lt;";   break;
        case '>':  escaped += "&gt;";   break;
        case '"':  escaped += "&quot;"; break;
        case '\'': escaped += "&#39;";  break;
        default:   escaped.push_back(character); break;
        }
    }

    return escaped;
}

std::string isoDate(Timestamp timestamp)
{
    const std::string value = std::to_string(timestamp);

    if (value.size() != 8U) {
        return value;
    }

    return value.substr(0, 4) + "-" + value.substr(4, 2) + "-" + value.substr(6, 2);
}

std::string formatNumber(double value, int decimals = 2)
{
    if (!std::isfinite(value)) {
        return "N/A";
    }

    std::ostringstream output;
    output << std::fixed << std::setprecision(decimals) << value;
    return output.str();
}

std::string formatCurrency(double value)
{
    return "$" + formatNumber(value, 2);
}

std::string formatPercent(double value)
{
    return formatNumber(value, 2) + "%";
}

std::string formatSignedPercent(double value)
{
    if (!std::isfinite(value)) {
        return "N/A";
    }

    const std::string sign = value > 0.0 ? "+" : "";
    return sign + formatPercent(value);
}

std::string formatDuration(
    std::size_t bars,
    double days
)
{
    if (days > 0.0 && std::isfinite(days)) {
        return formatNumber(days, 0) + " days (" + std::to_string(bars) + " bars)";
    }

    return std::to_string(bars) + " bars";
}

std::vector<EquityPoint> alignEquityCurve(
    const std::vector<std::pair<Balance, Equity>>& balanceEquityHistoric,
    const MarketData& marketData
)
{
    const std::size_t pointCount = std::min(
        balanceEquityHistoric.size(),
        marketData.size()
    );

    std::vector<EquityPoint> points;
    points.reserve(pointCount);

    auto marketIt = marketData.begin();
    for (std::size_t index = 0; index < pointCount; ++index, ++marketIt) {
        points.push_back(EquityPoint{
            marketIt->first,
            balanceEquityHistoric[index].second
        });
    }

    return points;
}

std::vector<EquityPoint> withInitialPoint(
    const std::vector<EquityPoint>& points,
    double initialEquity
)
{
    std::vector<EquityPoint> chartPoints;
    chartPoints.reserve(points.size() + 1U);

    if (!points.empty()) {
        chartPoints.push_back(EquityPoint{points.front().timestamp, initialEquity});
        chartPoints.insert(chartPoints.end(), points.begin(), points.end());
    }

    return chartPoints;
}

std::string makeMetricRow(
    const std::string& metric,
    const std::string& value
)
{
    std::ostringstream html;
    html << "<div class=\"metric-row\">"
         << "<span class=\"metric-label\">" << htmlEscape(metric) << "</span>"
         << "<span class=\"metric-value\">" << htmlEscape(value) << "</span>"
         << "</div>\n";
    return html.str();
}

std::string makeMetricColumn(
    const std::string& title,
    const std::vector<std::pair<std::string, std::string>>& rows
)
{
    std::ostringstream html;
    html << "<section class=\"metric-column\">\n"
         << "  <h2>" << htmlEscape(title) << "</h2>\n";

    for (const auto& [metric, value] : rows) {
        html << makeMetricRow(metric, value);
    }

    html << "</section>\n";
    return html.str();
}

std::string makeUnavailableSvg(const std::string& message)
{
    return "<svg viewBox=\"0 0 1000 420\" role=\"img\" aria-label=\"Chart unavailable\">\n"
           "  <rect x=\"0\" y=\"0\" width=\"1000\" height=\"420\" fill=\"#ffffff\"/>\n"
           "  <text x=\"500\" y=\"210\" text-anchor=\"middle\" font-family=\"Arial, sans-serif\" font-size=\"20\" fill=\"#64748b\">" +
           htmlEscape(message) +
           "</text>\n</svg>\n";
}

std::string makeSvgLineChart(
    const std::vector<EquityPoint>& points,
    const std::vector<double>& values,
    const std::string& ariaLabel,
    const std::string& lineColor,
    double minimumValue,
    double maximumValue,
    bool showDashedZeroLine,
    const std::string& zeroLineLabel
)
{
    constexpr double width = 1000.0;
    constexpr double height = 420.0;
    constexpr double left = 78.0;
    constexpr double right = 28.0;
    constexpr double top = 42.0;
    constexpr double bottom = 62.0;
    constexpr int gridLines = 5;

    if (points.empty() || points.size() != values.size()) {
        return makeUnavailableSvg("No equity points available");
    }

    for (const double value : values) {
        if (!std::isfinite(value)) {
            return makeUnavailableSvg("Invalid chart values");
        }
    }

    const double plotWidth = width - left - right;
    const double plotHeight = height - top - bottom;
    const double valueRange = std::max(maximumValue - minimumValue, 1e-12);
    const std::size_t lastIndex = points.size() - 1U;

    auto xFor = [&](std::size_t index) {
        if (lastIndex == 0U) {
            return left + plotWidth / 2.0;
        }

        return left + (static_cast<double>(index) / static_cast<double>(lastIndex)) * plotWidth;
    };

    auto yFor = [&](double value) {
        return top + ((maximumValue - value) / valueRange) * plotHeight;
    };

    std::ostringstream svg;
    svg << std::fixed << std::setprecision(2);
    svg << "<svg viewBox=\"0 0 1000 420\" role=\"img\" aria-label=\""
        << htmlEscape(ariaLabel) << "\" xmlns=\"http://www.w3.org/2000/svg\">\n";
    svg << "  <rect x=\"0\" y=\"0\" width=\"1000\" height=\"420\" fill=\"#ffffff\"/>\n";

    for (int gridIndex = 0; gridIndex <= gridLines; ++gridIndex) {
        const double ratio = static_cast<double>(gridIndex) / static_cast<double>(gridLines);
        const double y = top + ratio * plotHeight;
        const double value = maximumValue - ratio * valueRange;

        svg << "  <line x1=\"" << left << "\" y1=\"" << y
            << "\" x2=\"" << (left + plotWidth) << "\" y2=\"" << y
            << "\" stroke=\"#e2e8f0\" stroke-width=\"1\"/>\n";
        svg << "  <text x=\"" << (left - 10.0) << "\" y=\"" << (y + 4.0)
            << "\" text-anchor=\"end\" font-family=\"Arial, sans-serif\" font-size=\"12\" "
            << "fill=\"#64748b\">" << htmlEscape(formatSignedPercent(value)) << "</text>\n";
    }

    if (showDashedZeroLine && minimumValue <= 0.0 && maximumValue >= 0.0) {
        const double zeroY = yFor(0.0);
        svg << "  <line x1=\"" << left << "\" y1=\"" << zeroY
            << "\" x2=\"" << (left + plotWidth) << "\" y2=\"" << zeroY
            << "\" stroke=\"#94a3b8\" stroke-width=\"1.5\" stroke-dasharray=\"6 5\"/>\n";
        if (!zeroLineLabel.empty()) {
            svg << "  <text x=\"" << (left + plotWidth - 2.0) << "\" y=\"" << (zeroY - 7.0)
                << "\" text-anchor=\"end\" font-family=\"Arial, sans-serif\" font-size=\"12\" fill=\"#64748b\">"
                << htmlEscape(zeroLineLabel) << "</text>\n";
        }
    }

    svg << "  <polyline fill=\"none\" stroke=\"" << lineColor << "\" stroke-width=\"2.5\" "
        << "stroke-linejoin=\"round\" stroke-linecap=\"round\" points=\"";

    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index > 0U) {
            svg << " ";
        }
        svg << xFor(index) << "," << yFor(values[index]);
    }

    svg << "\"/>\n";
    svg << "  <circle cx=\"" << xFor(0U) << "\" cy=\"" << yFor(values.front())
        << "\" r=\"3.5\" fill=\"" << lineColor << "\"/>\n";
    svg << "  <circle cx=\"" << xFor(lastIndex) << "\" cy=\"" << yFor(values.back())
        << "\" r=\"4.0\" fill=\"" << lineColor << "\"/>\n";

    svg << "  <text x=\"" << left << "\" y=\"" << (height - 22.0)
        << "\" font-family=\"Arial, sans-serif\" font-size=\"12\" fill=\"#64748b\">"
        << htmlEscape(isoDate(points.front().timestamp)) << "</text>\n";
    svg << "  <text x=\"" << (left + plotWidth) << "\" y=\"" << (height - 22.0)
        << "\" text-anchor=\"end\" font-family=\"Arial, sans-serif\" font-size=\"12\" fill=\"#64748b\">"
        << htmlEscape(isoDate(points.back().timestamp)) << "</text>\n";
    svg << "</svg>\n";

    return svg.str();
}

std::string makeSvgEquityCurve(
    const std::vector<EquityPoint>& points,
    double initialEquity
)
{
    if (initialEquity <= 0.0 || !std::isfinite(initialEquity)) {
        return makeUnavailableSvg("Invalid initial equity");
    }

    const std::vector<EquityPoint> chartPoints = withInitialPoint(points, initialEquity);
    if (chartPoints.empty()) {
        return makeUnavailableSvg("No equity points available");
    }

    std::vector<double> returnsPercent;
    returnsPercent.reserve(chartPoints.size());

    double minimumReturn = 0.0;
    double maximumReturn = 0.0;

    for (const EquityPoint& point : chartPoints) {
        const double value = ((point.equity / initialEquity) - 1.0) * 100.0;
        returnsPercent.push_back(value);
        minimumReturn = std::min(minimumReturn, value);
        maximumReturn = std::max(maximumReturn, value);
    }

    const double rawRange = maximumReturn - minimumReturn;
    const double padding = rawRange > 0.0
        ? rawRange * 0.08
        : std::max(std::abs(maximumReturn) * 0.03, 1.0);

    return makeSvgLineChart(
        chartPoints,
        returnsPercent,
        "Equity curve as percentage return",
        "#2563eb",
        minimumReturn - padding,
        maximumReturn + padding,
        true,
        "0.00% baseline"
    );
}

std::string makeSvgDrawdownCurve(
    const std::vector<EquityPoint>& points,
    double initialEquity
)
{
    constexpr double width = 1000.0;
    constexpr double height = 420.0;
    constexpr double left = 78.0;
    constexpr double right = 28.0;
    constexpr double top = 42.0;
    constexpr double bottom = 62.0;
    constexpr int gridLines = 5;

    if (initialEquity <= 0.0 || !std::isfinite(initialEquity)) {
        return makeUnavailableSvg("Invalid initial equity");
    }

    const std::vector<EquityPoint> chartPoints =
        withInitialPoint(points, initialEquity);

    if (chartPoints.empty()) {
        return makeUnavailableSvg("No equity points available");
    }

    std::vector<double> drawdownPercent;
    drawdownPercent.reserve(chartPoints.size());

    double peakEquity = initialEquity;
    double minimumDrawdown = 0.0;

    for (const EquityPoint& point : chartPoints) {
        peakEquity = std::max(peakEquity, point.equity);

        const double drawdown = peakEquity > 0.0
            ? ((point.equity / peakEquity) - 1.0) * 100.0
            : 0.0;

        drawdownPercent.push_back(drawdown);
        minimumDrawdown = std::min(minimumDrawdown, drawdown);
    }

    const double padding =
        std::max(std::abs(minimumDrawdown) * 0.08, 1.0);

    const double minimumValue = minimumDrawdown - padding;
    const double maximumValue = std::max(0.0, padding * 0.20);

    const double plotWidth = width - left - right;
    const double plotHeight = height - top - bottom;
    const double valueRange =
        std::max(maximumValue - minimumValue, 1e-12);

    const std::size_t lastIndex = chartPoints.size() - 1U;

    auto xFor = [&](std::size_t index) {
        if (lastIndex == 0U) {
            return left + plotWidth / 2.0;
        }

        return left +
            (static_cast<double>(index) /
             static_cast<double>(lastIndex)) * plotWidth;
    };

    auto yFor = [&](double value) {
        return top +
            ((maximumValue - value) / valueRange) * plotHeight;
    };

    const double zeroY = yFor(0.0);

    std::ostringstream svg;
    svg << std::fixed << std::setprecision(2);

    svg << "<svg viewBox=\"0 0 1000 420\" role=\"img\" "
        << "aria-label=\"Drawdown from prior equity peak\" "
        << "xmlns=\"http://www.w3.org/2000/svg\">\n";

    svg << "  <defs>\n";
    svg << "    <linearGradient id=\"drawdownFillGradient\" "
        << "x1=\"0\" y1=\"0\" x2=\"0\" y2=\"1\">\n";
    svg << "      <stop offset=\"0%\" "
        << "stop-color=\"#dc2626\" stop-opacity=\"0.03\"/>\n";
    svg << "      <stop offset=\"100%\" "
        << "stop-color=\"#dc2626\" stop-opacity=\"0.34\"/>\n";
    svg << "    </linearGradient>\n";
    svg << "  </defs>\n";

    svg << "  <rect x=\"0\" y=\"0\" width=\"1000\" height=\"420\" "
        << "fill=\"#ffffff\"/>\n";

    for (int gridIndex = 0; gridIndex <= gridLines; ++gridIndex) {
        const double ratio =
            static_cast<double>(gridIndex) /
            static_cast<double>(gridLines);

        const double y = top + ratio * plotHeight;
        const double value = maximumValue - ratio * valueRange;

        svg << "  <line x1=\"" << left
            << "\" y1=\"" << y
            << "\" x2=\"" << (left + plotWidth)
            << "\" y2=\"" << y
            << "\" stroke=\"#e2e8f0\" stroke-width=\"1\"/>\n";

        svg << "  <text x=\"" << (left - 10.0)
            << "\" y=\"" << (y + 4.0)
            << "\" text-anchor=\"end\" "
            << "font-family=\"Arial, sans-serif\" "
            << "font-size=\"12\" fill=\"#64748b\">"
            << htmlEscape(formatSignedPercent(value))
            << "</text>\n";
    }

    svg << "  <path d=\"M " << xFor(0U) << " " << zeroY;

    for (std::size_t index = 0; index < drawdownPercent.size(); ++index) {
        svg << " L "
            << xFor(index)
            << " "
            << yFor(drawdownPercent[index]);
    }

    svg << " L "
        << xFor(lastIndex)
        << " "
        << zeroY
        << " Z\" fill=\"url(#drawdownFillGradient)\" "
        << "stroke=\"none\"/>\n";

    svg << "  <line x1=\"" << left
        << "\" y1=\"" << zeroY
        << "\" x2=\"" << (left + plotWidth)
        << "\" y2=\"" << zeroY
        << "\" stroke=\"#94a3b8\" stroke-width=\"1.5\" "
        << "stroke-dasharray=\"6 5\"/>\n";

    svg << "  <text x=\"" << (left + plotWidth - 2.0)
        << "\" y=\"" << (zeroY - 7.0)
        << "\" text-anchor=\"end\" "
        << "font-family=\"Arial, sans-serif\" "
        << "font-size=\"12\" fill=\"#64748b\">"
        << "0.00% peak</text>\n";

    svg << "  <polyline fill=\"none\" "
        << "stroke=\"#dc2626\" stroke-width=\"2.5\" "
        << "stroke-linejoin=\"round\" stroke-linecap=\"round\" "
        << "points=\"";

    for (std::size_t index = 0; index < drawdownPercent.size(); ++index) {
        if (index > 0U) {
            svg << " ";
        }

        svg << xFor(index)
            << ","
            << yFor(drawdownPercent[index]);
    }

    svg << "\"/>\n";

    svg << "  <circle cx=\"" << xFor(0U)
        << "\" cy=\"" << yFor(drawdownPercent.front())
        << "\" r=\"3.5\" fill=\"#dc2626\"/>\n";

    svg << "  <circle cx=\"" << xFor(lastIndex)
        << "\" cy=\"" << yFor(drawdownPercent.back())
        << "\" r=\"4.0\" fill=\"#dc2626\"/>\n";

    svg << "  <text x=\"" << left
        << "\" y=\"" << (height - 22.0)
        << "\" font-family=\"Arial, sans-serif\" "
        << "font-size=\"12\" fill=\"#64748b\">"
        << htmlEscape(isoDate(chartPoints.front().timestamp))
        << "</text>\n";

    svg << "  <text x=\"" << (left + plotWidth)
        << "\" y=\"" << (height - 22.0)
        << "\" text-anchor=\"end\" "
        << "font-family=\"Arial, sans-serif\" "
        << "font-size=\"12\" fill=\"#64748b\">"
        << htmlEscape(isoDate(chartPoints.back().timestamp))
        << "</text>\n";

    svg << "</svg>\n";

    return svg.str();
}

std::string makeSvgMonteCarloDrawdownHistogram(
    const BacktestMetrics& metrics
)
{
    constexpr double width = 1000.0;
    constexpr double height = 620.0;
    constexpr double left = 78.0;
    constexpr double right = 28.0;

    // The chart is taller so the reference labels have their own clear band
    // above the plotting area instead of overlapping the bars.
    constexpr double top = 170.0;
    constexpr double bottom = 74.0;
    constexpr double labelTop = 48.0;
    constexpr double labelRowSpacing = 30.0;
    constexpr std::size_t labelRowCount = 4U;

    constexpr int gridLines = 5;
    constexpr std::size_t binCount = 20U;
    constexpr double drawdownCap = 100.0;

    if (metrics.monteCarloMaxDrawdownPercentSamples.empty()) {
        return makeUnavailableSvg("Monte Carlo results unavailable");
    }

    // A non-leveraged account cannot lose more than 100% of its equity.
    // Values at/above 100% are rendered in the last overflow / ruin bin.
    std::vector<double> samples;
    samples.reserve(metrics.monteCarloMaxDrawdownPercentSamples.size());

    for (const double value : metrics.monteCarloMaxDrawdownPercentSamples) {
        if (std::isfinite(value) && value >= 0.0) {
            samples.push_back(std::min(value, drawdownCap));
        }
    }

    if (samples.empty()) {
        return makeUnavailableSvg("Monte Carlo results unavailable");
    }

    const double plotWidth = width - left - right;
    const double plotHeight = height - top - bottom;
    const double binWidthValue = drawdownCap / static_cast<double>(binCount);

    std::vector<std::size_t> binCounts(binCount, 0U);

    for (const double sample : samples) {
        std::size_t binIndex = static_cast<std::size_t>(
            std::floor((sample / drawdownCap) * static_cast<double>(binCount))
        );

        if (binIndex >= binCount) {
            binIndex = binCount - 1U;
        }

        ++binCounts[binIndex];
    }

    std::vector<double> binPercentages(binCount, 0.0);
    double maximumBinPercent = 0.0;

    for (std::size_t index = 0; index < binCount; ++index) {
        binPercentages[index] =
            (static_cast<double>(binCounts[index]) /
             static_cast<double>(samples.size())) * 100.0;

        maximumBinPercent = std::max(maximumBinPercent, binPercentages[index]);
    }

    const double maximumY = std::max(
        1.0,
        std::ceil(maximumBinPercent * 1.15)
    );

    auto xFor = [&](double drawdownPercent) {
        const double clampedValue = std::clamp(
            drawdownPercent,
            0.0,
            drawdownCap
        );

        return left + (clampedValue / drawdownCap) * plotWidth;
    };

    auto yFor = [&](double simulationPercent) {
        return top +
            ((maximumY - simulationPercent) / maximumY) * plotHeight;
    };

    auto formatCappedDrawdown = [&](double value) {
        if (!std::isfinite(value)) {
            return std::string("N/A");
        }

        if (value >= drawdownCap - 1e-9) {
            return std::string("100.00%+");
        }

        return formatPercent(std::max(0.0, value));
    };

    struct Marker {
        double value = 0.0;
        std::string label;
        const char* color = "#0f172a";
        const char* dashArray = "2 3";
        double strokeWidth = 1.8;
        std::size_t labelRow = 0U;
    };

    const std::vector<Marker> markers{
        {
            metrics.maxDrawdownPercent,
            "Historical (" + formatCappedDrawdown(metrics.maxDrawdownPercent) + ")",
            "#0f172a",
            "2 3",
            1.8,
            0U
        },
        {
            metrics.monteCarloMedianMaxDrawdownPercent,
            "Median (" + formatCappedDrawdown(metrics.monteCarloMedianMaxDrawdownPercent) + ")",
            "#2563eb",
            "2 3",
            1.8,
            1U
        },
        {
            metrics.monteCarloP95MaxDrawdownPercent,
            "P95 (" + formatCappedDrawdown(metrics.monteCarloP95MaxDrawdownPercent) + ")",
            "#7c3aed",
            "2 3",
            1.8,
            2U
        },
        {
            metrics.monteCarloP99MaxDrawdownPercent,
            "P99 (" + formatCappedDrawdown(metrics.monteCarloP99MaxDrawdownPercent) + ")",
            "#be123c",
            "2 3",
            1.8,
            3U
        }
    };

    std::ostringstream svg;
    svg << std::fixed << std::setprecision(2);

    svg << "<svg viewBox=\"0 0 1000 620\" role=\"img\" "
        << "aria-label=\"Monte Carlo maximum drawdown distribution\" "
        << "xmlns=\"http://www.w3.org/2000/svg\">\n";

    svg << "  <defs>\n";
    svg << "    <linearGradient id=\"monteCarloHistogramBar\" "
        << "x1=\"0\" y1=\"0\" x2=\"0\" y2=\"1\">\n";
    svg << "      <stop offset=\"0%\" stop-color=\"#60a5fa\" stop-opacity=\"0.90\"/>\n";
    svg << "      <stop offset=\"100%\" stop-color=\"#2563eb\" stop-opacity=\"0.74\"/>\n";
    svg << "    </linearGradient>\n";
    svg << "  </defs>\n";

    svg << "  <rect x=\"0\" y=\"0\" width=\"1000\" height=\"620\" fill=\"#ffffff\"/>\n";

    // A visibly separate reference-label lane, attached to the graph by the
    // matching dashed vertical line for each statistic.
    svg << "  <rect x=\"" << left
        << "\" y=\"20\" width=\"" << plotWidth
        << "\" height=\"" << (top - 36.0)
        << "\" rx=\"8\" fill=\"#fbfdff\"/>\n";
    svg << "  <line x1=\"" << left
        << "\" y1=\"" << (top - 14.0)
        << "\" x2=\"" << (left + plotWidth)
        << "\" y2=\"" << (top - 14.0)
        << "\" stroke=\"#e2e8f0\" stroke-width=\"1\"/>\n";

    for (int gridIndex = 0; gridIndex <= gridLines; ++gridIndex) {
        const double ratio =
            static_cast<double>(gridIndex) / static_cast<double>(gridLines);
        const double y = top + ratio * plotHeight;
        const double simulationPercent = maximumY - ratio * maximumY;

        svg << "  <line x1=\"" << left
            << "\" y1=\"" << y
            << "\" x2=\"" << (left + plotWidth)
            << "\" y2=\"" << y
            << "\" stroke=\"#e2e8f0\" stroke-width=\"1\"/>\n";

        svg << "  <text x=\"" << (left - 10.0)
            << "\" y=\"" << (y + 4.0)
            << "\" text-anchor=\"end\" "
            << "font-family=\"Arial, sans-serif\" "
            << "font-size=\"12\" fill=\"#64748b\">"
            << formatNumber(simulationPercent, 0)
            << "%</text>\n";
    }

    const double barGap = 2.0;
    const double barWidth =
        std::max(1.0, (plotWidth / static_cast<double>(binCount)) - barGap);

    for (std::size_t index = 0; index < binCount; ++index) {
        const double binStart = static_cast<double>(index) * binWidthValue;
        const double binEnd = binStart + binWidthValue;
        const double x = xFor(binStart) + barGap / 2.0;
        const double y = yFor(binPercentages[index]);
        const double barHeight = (top + plotHeight) - y;
        const bool isRuinBin = index == binCount - 1U;

        std::string rangeLabel;
        if (isRuinBin) {
            rangeLabel =
                formatNumber(binStart, 1) + "% to 100.0%+ / ruin";
        } else {
            rangeLabel =
                formatNumber(binStart, 1) + "% to " +
                formatNumber(binEnd, 1) + "%";
        }

        svg << "  <g>\n";
        svg << "    <title>Maximum drawdown "
            << rangeLabel
            << ": "
            << formatNumber(binPercentages[index], 2)
            << "% of simulations</title>\n";
        svg << "    <rect x=\"" << x
            << "\" y=\"" << y
            << "\" width=\"" << barWidth
            << "\" height=\"" << barHeight
            << "\" rx=\"1.5\" fill=\"url(#monteCarloHistogramBar)\"/>\n";
        svg << "  </g>\n";
    }

    // Reference labels use four separate rows. Their dashed lines start just
    // below the label, visually linking the number to the exact x location.
    for (const Marker& marker : markers) {
        if (!std::isfinite(marker.value) || marker.value < 0.0) {
            continue;
        }

        const double plottedValue = std::min(marker.value, drawdownCap);
        const double x = xFor(plottedValue);
        const double labelY =
            labelTop +
            static_cast<double>(marker.labelRow % labelRowCount) *
                labelRowSpacing;

        const bool nearLeft = plottedValue <= 7.0;
        const bool nearRight = plottedValue >= 93.0;
        const double labelX = nearLeft ? x + 5.0 : (nearRight ? x - 5.0 : x);
        const char* textAnchor = nearLeft ? "start" : (nearRight ? "end" : "middle");

        svg << "  <text x=\"" << labelX
            << "\" y=\"" << labelY
            << "\" text-anchor=\"" << textAnchor << "\" "
            << "font-family=\"Arial, sans-serif\" "
            << "font-size=\"12\" font-weight=\"700\" "
            << "fill=\"" << marker.color << "\">"
            << htmlEscape(marker.label)
            << "</text>\n";

        svg << "  <line x1=\"" << x
            << "\" y1=\"" << (labelY + 10.0)
            << "\" x2=\"" << x
            << "\" y2=\"" << (top + plotHeight)
            << "\" stroke=\"" << marker.color
            << "\" stroke-width=\"" << marker.strokeWidth
            << "\" stroke-dasharray=\"" << marker.dashArray
            << "\"/>\n";
    }

    const std::vector<double> xTickValues{0.0, 20.0, 40.0, 60.0, 80.0, 100.0};

    for (const double value : xTickValues) {
        const double x = xFor(value);
        const std::string label = value >= drawdownCap
            ? "100%+"
            : formatNumber(value, 0) + "%";

        svg << "  <line x1=\"" << x
            << "\" y1=\"" << (top + plotHeight)
            << "\" x2=\"" << x
            << "\" y2=\"" << (top + plotHeight + 5.0)
            << "\" stroke=\"#94a3b8\" stroke-width=\"1\"/>\n";

        svg << "  <text x=\"" << x
            << "\" y=\"" << (top + plotHeight + 24.0)
            << "\" text-anchor=\"middle\" "
            << "font-family=\"Arial, sans-serif\" "
            << "font-size=\"12\" fill=\"#64748b\">"
            << label
            << "</text>\n";
    }

    svg << "  <text x=\"" << (left + plotWidth / 2.0)
        << "\" y=\"" << (top + plotHeight + 48.0)
        << "\" text-anchor=\"middle\" "
        << "font-family=\"Arial, sans-serif\" "
        << "font-size=\"12\" fill=\"#64748b\">"
        << "Simulated maximum drawdown (100%+ = simulated ruin)"
        << "</text>\n";

    svg << "</svg>\n";

    return svg.str();
}


std::string makeSvgHistoricalTradeReturnHistogram(
    const BacktestMetrics& metrics
)
{
    constexpr double width = 1000.0;
    constexpr double height = 600.0;
    constexpr double left = 78.0;
    constexpr double right = 28.0;
    constexpr double top = 118.0;
    constexpr double bottom = 98.0;
    constexpr double labelTop = 42.0;
    constexpr double labelRowSpacing = 30.0;
    constexpr int gridLines = 5;
    constexpr std::size_t preliminaryBinCount = 120U;
    constexpr std::size_t minimumDisplayBinCount = 32U;
    constexpr std::size_t maximumDisplayBinCount = 40U;
    constexpr double minimumVisibleBinProbabilityPercent = 0.5;
    constexpr int xTickCount = 10;

    if (metrics.historicalTradeReturnPercentSamples.empty()) {
        return makeUnavailableSvg("Historical trade-return results unavailable");
    }

    std::vector<double> samples;
    samples.reserve(metrics.historicalTradeReturnPercentSamples.size());

    for (const double value : metrics.historicalTradeReturnPercentSamples) {
        if (std::isfinite(value)) {
            samples.push_back(value);
        }
    }

    if (samples.empty()) {
        return makeUnavailableSvg("Historical trade-return results unavailable");
    }

    const auto [minimumIt, maximumIt] = std::minmax_element(
        samples.begin(),
        samples.end()
    );

    double rawMinimumReturn = *minimumIt;
    double rawMaximumReturn = *maximumIt;

    if (rawMaximumReturn - rawMinimumReturn < 1e-9) {
        const double singleValuePadding = std::max(
            std::abs(rawMaximumReturn) * 0.08,
            1.0
        );
        rawMinimumReturn -= singleValuePadding;
        rawMaximumReturn += singleValuePadding;
    }

    const double rawReturnRange = std::max(
        rawMaximumReturn - rawMinimumReturn,
        1e-12
    );
    const double preliminaryBinWidth = rawReturnRange /
        static_cast<double>(preliminaryBinCount);

    // First locate the central portion of the distribution. Rare tail bins
    // below 0.5% of all closed trades are excluded from the displayed x-range,
    // so isolated outliers do not stretch the chart.
    std::vector<std::size_t> preliminaryBinCounts(preliminaryBinCount, 0U);
    for (const double sample : samples) {
        std::size_t binIndex = static_cast<std::size_t>(
            std::floor(
                (sample - rawMinimumReturn) / rawReturnRange *
                static_cast<double>(preliminaryBinCount)
            )
        );
        if (binIndex >= preliminaryBinCount) {
            binIndex = preliminaryBinCount - 1U;
        }
        ++preliminaryBinCounts[binIndex];
    }

    bool foundVisibleRange = false;
    std::size_t firstVisiblePreliminaryBin = 0U;
    std::size_t lastVisiblePreliminaryBin = preliminaryBinCount - 1U;

    for (std::size_t index = 0U; index < preliminaryBinCount; ++index) {
        const double probabilityPercent =
            (static_cast<double>(preliminaryBinCounts[index]) /
             static_cast<double>(samples.size())) * 100.0;

        if (probabilityPercent + 1e-12 >= minimumVisibleBinProbabilityPercent) {
            if (!foundVisibleRange) {
                firstVisiblePreliminaryBin = index;
                foundVisibleRange = true;
            }
            lastVisiblePreliminaryBin = index;
        }
    }

    double minimumReturn = rawMinimumReturn;
    double maximumReturn = rawMaximumReturn;

    if (foundVisibleRange) {
        minimumReturn = rawMinimumReturn +
            static_cast<double>(firstVisiblePreliminaryBin) * preliminaryBinWidth;
        maximumReturn = rawMinimumReturn +
            static_cast<double>(lastVisiblePreliminaryBin + 1U) * preliminaryBinWidth;

        // Keep a small breathing margin while still clipping rare tails.
        const double visibleRange = std::max(maximumReturn - minimumReturn, 1e-12);
        const double visiblePadding = std::min(
            std::max(visibleRange * 0.025, preliminaryBinWidth * 0.50),
            rawReturnRange * 0.05
        );
        minimumReturn = std::max(rawMinimumReturn, minimumReturn - visiblePadding);
        maximumReturn = std::min(rawMaximumReturn, maximumReturn + visiblePadding);
    }

    // Keep the two reference statistics visible even when a rare tail was
    // clipped from the histogram range.
    for (const double referenceValue : {
             metrics.historicalMedianTradeReturnPercent,
             metrics.historicalMeanTradeReturnPercent
         }) {
        if (std::isfinite(referenceValue)) {
            minimumReturn = std::min(minimumReturn, referenceValue);
            maximumReturn = std::max(maximumReturn, referenceValue);
        }
    }

    if (maximumReturn - minimumReturn < 2.0) {
        const double center = (minimumReturn + maximumReturn) * 0.5;
        minimumReturn = center - 1.0;
        maximumReturn = center + 1.0;
    }

    const std::size_t binCount = std::min(
        maximumDisplayBinCount,
        std::max(minimumDisplayBinCount, samples.size() / 3U)
    );

    const double plotWidth = width - left - right;
    const double plotHeight = height - top - bottom;
    const double returnRange = std::max(maximumReturn - minimumReturn, 1e-12);
    const double binWidth = returnRange / static_cast<double>(binCount);

    std::vector<std::size_t> binCounts(binCount, 0U);
    std::size_t outOfDisplayedRangeCount = 0U;

    for (const double sample : samples) {
        if (sample < minimumReturn || sample > maximumReturn) {
            ++outOfDisplayedRangeCount;
            continue;
        }

        std::size_t binIndex = static_cast<std::size_t>(
            std::floor(
                (sample - minimumReturn) / returnRange *
                static_cast<double>(binCount)
            )
        );
        if (binIndex >= binCount) {
            binIndex = binCount - 1U;
        }
        ++binCounts[binIndex];
    }

    std::vector<double> binPercentages(binCount, 0.0);
    std::vector<bool> showBin(binCount, false);
    double maximumBinPercent = 0.0;
    double displayedProbabilityPercent = 0.0;

    for (std::size_t index = 0U; index < binCount; ++index) {
        binPercentages[index] =
            (static_cast<double>(binCounts[index]) /
             static_cast<double>(samples.size())) * 100.0;

        showBin[index] =
            binPercentages[index] + 1e-12 >= minimumVisibleBinProbabilityPercent;

        if (showBin[index]) {
            maximumBinPercent = std::max(maximumBinPercent, binPercentages[index]);
            displayedProbabilityPercent += binPercentages[index];
        }
    }

    // A fallback keeps the chart usable for very small trade samples where
    // all display bins happen to sit below 0.5%.
    if (maximumBinPercent <= 0.0) {
        for (std::size_t index = 0U; index < binCount; ++index) {
            showBin[index] = binCounts[index] > 0U;
            if (showBin[index]) {
                maximumBinPercent = std::max(maximumBinPercent, binPercentages[index]);
                displayedProbabilityPercent += binPercentages[index];
            }
        }
    }

    const double maximumY = std::max(
        1.0,
        std::ceil(maximumBinPercent * 1.15)
    );

    auto xFor = [&](double value) {
        const double clampedValue = std::clamp(value, minimumReturn, maximumReturn);
        return left + ((clampedValue - minimumReturn) / returnRange) * plotWidth;
    };

    auto yFor = [&](double value) {
        return top + ((maximumY - value) / maximumY) * plotHeight;
    };

    auto formatAxisPercent = [&](double value) {
        const int decimals = returnRange <= 20.0 ? 1 : 0;
        const std::string sign = value > 0.0 ? "+" : "";
        return sign + formatNumber(value, decimals) + "%";
    };

    struct Marker {
        double value = 0.0;
        std::string label;
        const char* color = "#0f172a";
        std::size_t labelRow = 0U;
    };

    const std::vector<Marker> markers{
        {
            metrics.historicalMedianTradeReturnPercent,
            "Median (" + formatSignedPercent(metrics.historicalMedianTradeReturnPercent) + ")",
            "#2563eb",
            0U
        },
        {
            metrics.historicalMeanTradeReturnPercent,
            "Mean (" + formatSignedPercent(metrics.historicalMeanTradeReturnPercent) + ")",
            "#0f172a",
            1U
        }
    };

    std::ostringstream svg;
    svg << std::fixed << std::setprecision(2);

    svg << "<svg viewBox=\"0 0 1000 600\" role=\"img\" "
        << "aria-label=\"Historical net return per trade distribution\" "
        << "xmlns=\"http://www.w3.org/2000/svg\">\n";
    svg << "  <defs>\n";
    svg << "    <linearGradient id=\"historicalTradeReturnBar\" x1=\"0\" y1=\"0\" x2=\"0\" y2=\"1\">\n";
    svg << "      <stop offset=\"0%\" stop-color=\"#34d399\" stop-opacity=\"0.90\"/>\n";
    svg << "      <stop offset=\"100%\" stop-color=\"#059669\" stop-opacity=\"0.74\"/>\n";
    svg << "    </linearGradient>\n";
    svg << "  </defs>\n";
    svg << "  <rect x=\"0\" y=\"0\" width=\"1000\" height=\"600\" fill=\"#ffffff\"/>\n";

    svg << "  <rect x=\"" << left << "\" y=\"20\" width=\"" << plotWidth
        << "\" height=\"" << (top - 34.0)
        << "\" rx=\"8\" fill=\"#fbfdff\"/>\n";
    svg << "  <line x1=\"" << left << "\" y1=\"" << (top - 14.0)
        << "\" x2=\"" << (left + plotWidth) << "\" y2=\"" << (top - 14.0)
        << "\" stroke=\"#e2e8f0\" stroke-width=\"1\"/>\n";

    for (int gridIndex = 0; gridIndex <= gridLines; ++gridIndex) {
        const double ratio = static_cast<double>(gridIndex) /
                             static_cast<double>(gridLines);
        const double y = top + ratio * plotHeight;
        const double probabilityPercent = maximumY - ratio * maximumY;

        svg << "  <line x1=\"" << left << "\" y1=\"" << y
            << "\" x2=\"" << (left + plotWidth) << "\" y2=\"" << y
            << "\" stroke=\"#e2e8f0\" stroke-width=\"1\"/>\n";
        svg << "  <text x=\"" << (left - 10.0) << "\" y=\"" << (y + 4.0)
            << "\" text-anchor=\"end\" font-family=\"Arial, sans-serif\" "
            << "font-size=\"12\" fill=\"#64748b\">"
            << formatNumber(probabilityPercent, 0) << "%</text>\n";
    }

    const double barGap = 2.0;
    const double barWidth = std::max(
        1.0,
        (plotWidth / static_cast<double>(binCount)) - barGap
    );

    for (std::size_t index = 0U; index < binCount; ++index) {
        if (!showBin[index]) {
            continue;
        }

        const double binStart = minimumReturn + static_cast<double>(index) * binWidth;
        const double binEnd = binStart + binWidth;
        const double x = xFor(binStart) + barGap / 2.0;
        const double y = yFor(binPercentages[index]);
        const double barHeight = (top + plotHeight) - y;

        svg << "  <g>\n";
        svg << "    <title>Net trade return " << formatSignedPercent(binStart)
            << " to " << formatSignedPercent(binEnd) << ": "
            << formatNumber(binPercentages[index], 2)
            << "% of closed trades</title>\n";
        svg << "    <rect x=\"" << x << "\" y=\"" << y
            << "\" width=\"" << barWidth << "\" height=\"" << barHeight
            << "\" rx=\"1.5\" fill=\"url(#historicalTradeReturnBar)\"/>\n";
        svg << "  </g>\n";
    }

    if (minimumReturn <= 0.0 && maximumReturn >= 0.0) {
        const double zeroX = xFor(0.0);

        // Make the break-even reference unmistakable: a narrow white halo
        // separates it from the histogram bars, with a solid near-black line
        // on top. It is intentionally unlike the dashed mean/median markers.
        svg << "  <line x1=\"" << zeroX << "\" y1=\"" << top
            << "\" x2=\"" << zeroX << "\" y2=\"" << (top + plotHeight)
            << "\" stroke=\"#ffffff\" stroke-opacity=\"0.92\" stroke-width=\"5.0\"/>\n";
        svg << "  <line x1=\"" << zeroX << "\" y1=\"" << top
            << "\" x2=\"" << zeroX << "\" y2=\"" << (top + plotHeight)
            << "\" stroke=\"#111111\" stroke-width=\"2.4\" stroke-linecap=\"round\"/>\n";
    }

    for (const Marker& marker : markers) {
        if (!std::isfinite(marker.value) ||
            marker.value < minimumReturn || marker.value > maximumReturn) {
            continue;
        }

        const double x = xFor(marker.value);
        const double labelY = labelTop +
            static_cast<double>(marker.labelRow) * labelRowSpacing;
        const bool nearLeft = marker.value <= minimumReturn + returnRange * 0.05;
        const bool nearRight = marker.value >= maximumReturn - returnRange * 0.05;
        const double labelX = nearLeft ? x + 5.0 : (nearRight ? x - 5.0 : x);
        const char* textAnchor = nearLeft ? "start" : (nearRight ? "end" : "middle");

        svg << "  <text x=\"" << labelX << "\" y=\"" << labelY
            << "\" text-anchor=\"" << textAnchor << "\" "
            << "font-family=\"Arial, sans-serif\" font-size=\"12\" "
            << "font-weight=\"700\" fill=\"" << marker.color << "\">"
            << htmlEscape(marker.label) << "</text>\n";
        svg << "  <line x1=\"" << x << "\" y1=\"" << (labelY + 10.0)
            << "\" x2=\"" << x << "\" y2=\"" << (top + plotHeight)
            << "\" stroke=\"" << marker.color << "\" stroke-width=\"1.8\" "
            << "stroke-dasharray=\"2 3\"/>\n";
    }

    for (int tickIndex = 0; tickIndex <= xTickCount; ++tickIndex) {
        const double ratio = static_cast<double>(tickIndex) /
                             static_cast<double>(xTickCount);
        const double value = minimumReturn + ratio * returnRange;
        const double x = xFor(value);

        svg << "  <line x1=\"" << x << "\" y1=\"" << (top + plotHeight)
            << "\" x2=\"" << x << "\" y2=\"" << (top + plotHeight + 5.0)
            << "\" stroke=\"#94a3b8\" stroke-width=\"1\"/>\n";
        svg << "  <text x=\"" << x << "\" y=\"" << (top + plotHeight + 24.0)
            << "\" text-anchor=\"middle\" font-family=\"Arial, sans-serif\" "
            << "font-size=\"11\" fill=\"#64748b\">"
            << htmlEscape(formatAxisPercent(value)) << "</text>\n";
    }

    const double hiddenProbabilityPercent = std::max(
        0.0,
        100.0 - displayedProbabilityPercent
    );

    svg << "  <text x=\"" << (left + plotWidth / 2.0)
        << "\" y=\"" << (top + plotHeight + 50.0)
        << "\" text-anchor=\"middle\" font-family=\"Arial, sans-serif\" "
        << "font-size=\"12\" fill=\"#64748b\">"
        << "Net return per closed trade (% of entry notional)"
        << "</text>\n";
    svg << "  <text x=\"" << (left + plotWidth / 2.0)
        << "\" y=\"" << (top + plotHeight + 70.0)
        << "\" text-anchor=\"middle\" font-family=\"Arial, sans-serif\" "
        << "font-size=\"11\" fill=\"#94a3b8\">"
        << "Bars below 0.5% of closed trades are hidden; displayed bars cover "
        << formatNumber(displayedProbabilityPercent, 1)
        << "% of trades";

    if (outOfDisplayedRangeCount > 0U || hiddenProbabilityPercent > 0.05) {
        svg << " (" << formatNumber(hiddenProbabilityPercent, 1)
            << "% hidden)";
    }

    svg << "</text>\n";
    svg << "</svg>\n";

    return svg.str();
}

std::string makeSvgMonteCarloReturnFan(
    const BacktestMetrics& metrics
)
{
    constexpr double width = 1000.0;
    constexpr double height = 500.0;
    constexpr double left = 78.0;
    constexpr double right = 28.0;
    constexpr double top = 78.0;
    constexpr double bottom = 70.0;
    constexpr int gridLines = 5;

    const std::vector<MonteCarloFanPoint>& fan =
        metrics.monteCarloFanReturnPercent;

    if (fan.size() < 2U ||
        metrics.monteCarloHistoricalTradeReturnPercentPath.empty()) {
        return makeUnavailableSvg("Monte Carlo fan chart unavailable");
    }

    std::vector<double> historicalValues;
    historicalValues.reserve(fan.size());

    const std::vector<double>& historicalPath =
        metrics.monteCarloHistoricalTradeReturnPercentPath;

    auto historicalValueAt = [&](double progressPercent) {
        if (historicalPath.size() == 1U) {
            return historicalPath.front();
        }

        const double boundedProgress = std::clamp(progressPercent, 0.0, 100.0);
        const double position =
            (boundedProgress / 100.0) *
            static_cast<double>(historicalPath.size() - 1U);
        const std::size_t lowerIndex = static_cast<std::size_t>(std::floor(position));
        const std::size_t upperIndex = static_cast<std::size_t>(std::ceil(position));

        if (lowerIndex == upperIndex) {
            return historicalPath[lowerIndex];
        }

        const double weight = position - static_cast<double>(lowerIndex);
        return historicalPath[lowerIndex] * (1.0 - weight) +
               historicalPath[upperIndex] * weight;
    };

    double minimumReturn = 0.0;
    double maximumReturn = 0.0;

    for (const MonteCarloFanPoint& point : fan) {
        if (!std::isfinite(point.p05ReturnPercent) ||
            !std::isfinite(point.p25ReturnPercent) ||
            !std::isfinite(point.medianReturnPercent) ||
            !std::isfinite(point.p75ReturnPercent) ||
            !std::isfinite(point.p95ReturnPercent)) {
            return makeUnavailableSvg("Invalid Monte Carlo fan values");
        }

        const double historicalValue = historicalValueAt(point.progressPercent);
        historicalValues.push_back(historicalValue);

        minimumReturn = std::min({
            minimumReturn,
            point.p05ReturnPercent,
            point.p25ReturnPercent,
            point.medianReturnPercent,
            point.p75ReturnPercent,
            point.p95ReturnPercent,
            historicalValue
        });
        maximumReturn = std::max({
            maximumReturn,
            point.p05ReturnPercent,
            point.p25ReturnPercent,
            point.medianReturnPercent,
            point.p75ReturnPercent,
            point.p95ReturnPercent,
            historicalValue
        });
    }

    const double rawRange = maximumReturn - minimumReturn;
    const double padding = rawRange > 0.0
        ? rawRange * 0.08
        : std::max(std::abs(maximumReturn) * 0.08, 5.0);

    minimumReturn -= padding;
    maximumReturn += padding;

    const double plotWidth = width - left - right;
    const double plotHeight = height - top - bottom;
    const double returnRange = std::max(maximumReturn - minimumReturn, 1e-12);

    auto xFor = [&](double progressPercent) {
        return left + (std::clamp(progressPercent, 0.0, 100.0) / 100.0) * plotWidth;
    };

    auto yFor = [&](double returnPercent) {
        return top + ((maximumReturn - returnPercent) / returnRange) * plotHeight;
    };

    auto makeBandPath = [&](auto upperValue, auto lowerValue) {
        std::ostringstream path;
        path << std::fixed << std::setprecision(2);
        path << "M " << xFor(fan.front().progressPercent) << " "
             << yFor(upperValue(fan.front()));

        for (std::size_t index = 1U; index < fan.size(); ++index) {
            path << " L " << xFor(fan[index].progressPercent) << " "
                 << yFor(upperValue(fan[index]));
        }

        for (std::size_t reverseIndex = fan.size(); reverseIndex-- > 0U;) {
            path << " L " << xFor(fan[reverseIndex].progressPercent) << " "
                 << yFor(lowerValue(fan[reverseIndex]));
        }

        path << " Z";
        return path.str();
    };

    std::ostringstream svg;
    svg << std::fixed << std::setprecision(2);
    svg << "<svg viewBox=\"0 0 1000 500\" role=\"img\" "
        << "aria-label=\"Monte Carlo return fan chart\" "
        << "xmlns=\"http://www.w3.org/2000/svg\">\n";
    svg << "  <defs>\n";
    svg << "    <linearGradient id=\"fanOuterFill\" x1=\"0\" y1=\"0\" x2=\"0\" y2=\"1\">\n";
    svg << "      <stop offset=\"0%\" stop-color=\"#60a5fa\" stop-opacity=\"0.16\"/>\n";
    svg << "      <stop offset=\"100%\" stop-color=\"#2563eb\" stop-opacity=\"0.08\"/>\n";
    svg << "    </linearGradient>\n";
    svg << "    <linearGradient id=\"fanInnerFill\" x1=\"0\" y1=\"0\" x2=\"0\" y2=\"1\">\n";
    svg << "      <stop offset=\"0%\" stop-color=\"#2563eb\" stop-opacity=\"0.34\"/>\n";
    svg << "      <stop offset=\"100%\" stop-color=\"#1d4ed8\" stop-opacity=\"0.20\"/>\n";
    svg << "    </linearGradient>\n";
    svg << "  </defs>\n";
    svg << "  <rect x=\"0\" y=\"0\" width=\"1000\" height=\"500\" fill=\"#ffffff\"/>\n";

    for (int gridIndex = 0; gridIndex <= gridLines; ++gridIndex) {
        const double ratio = static_cast<double>(gridIndex) /
                             static_cast<double>(gridLines);
        const double y = top + ratio * plotHeight;
        const double value = maximumReturn - ratio * returnRange;

        svg << "  <line x1=\"" << left << "\" y1=\"" << y
            << "\" x2=\"" << (left + plotWidth) << "\" y2=\"" << y
            << "\" stroke=\"#e2e8f0\" stroke-width=\"1\"/>\n";
        svg << "  <text x=\"" << (left - 10.0) << "\" y=\"" << (y + 4.0)
            << "\" text-anchor=\"end\" font-family=\"Arial, sans-serif\" "
            << "font-size=\"12\" fill=\"#64748b\">"
            << htmlEscape(formatSignedPercent(value)) << "</text>\n";
    }

    if (minimumReturn <= 0.0 && maximumReturn >= 0.0) {
        const double zeroY = yFor(0.0);
        svg << "  <line x1=\"" << left << "\" y1=\"" << zeroY
            << "\" x2=\"" << (left + plotWidth) << "\" y2=\"" << zeroY
            << "\" stroke=\"#94a3b8\" stroke-width=\"1.5\" stroke-dasharray=\"6 5\"/>\n";
    }

    svg << "  <path d=\""
        << makeBandPath(
            [](const MonteCarloFanPoint& point) { return point.p95ReturnPercent; },
            [](const MonteCarloFanPoint& point) { return point.p05ReturnPercent; }
        )
        << "\" fill=\"url(#fanOuterFill)\" stroke=\"none\"/>\n";

    svg << "  <path d=\""
        << makeBandPath(
            [](const MonteCarloFanPoint& point) { return point.p75ReturnPercent; },
            [](const MonteCarloFanPoint& point) { return point.p25ReturnPercent; }
        )
        << "\" fill=\"url(#fanInnerFill)\" stroke=\"none\"/>\n";

    svg << "  <polyline fill=\"none\" stroke=\"#2563eb\" stroke-width=\"2.4\" "
        << "stroke-linejoin=\"round\" stroke-linecap=\"round\" points=\"";
    for (std::size_t index = 0U; index < fan.size(); ++index) {
        if (index > 0U) {
            svg << " ";
        }
        svg << xFor(fan[index].progressPercent) << ","
            << yFor(fan[index].medianReturnPercent);
    }
    svg << "\"/>\n";

    svg << "  <polyline fill=\"none\" stroke=\"#0f172a\" stroke-width=\"2.0\" "
        << "stroke-linejoin=\"round\" stroke-linecap=\"round\" points=\"";
    for (std::size_t index = 0U; index < fan.size(); ++index) {
        if (index > 0U) {
            svg << " ";
        }
        svg << xFor(fan[index].progressPercent) << ","
            << yFor(historicalValues[index]);
    }
    svg << "\"/>\n";

    // Compact in-chart legend.
    svg << "  <rect x=\"" << left << "\" y=\"18\" width=\""
        << plotWidth << "\" height=\"38\" rx=\"8\" fill=\"#fbfdff\"/>\n";
    svg << "  <line x1=\"" << (left + 16.0) << "\" y1=\"36\" x2=\""
        << (left + 42.0) << "\" y2=\"36\" stroke=\"#0f172a\" stroke-width=\"2.0\"/>\n";
    svg << "  <text x=\"" << (left + 49.0) << "\" y=\"40\" font-family=\"Arial, sans-serif\" "
        << "font-size=\"12\" fill=\"#0f172a\">Historical closed-trade return</text>\n";
    svg << "  <line x1=\"" << (left + 272.0) << "\" y1=\"36\" x2=\""
        << (left + 298.0) << "\" y2=\"36\" stroke=\"#2563eb\" stroke-width=\"2.4\"/>\n";
    svg << "  <text x=\"" << (left + 305.0) << "\" y=\"40\" font-family=\"Arial, sans-serif\" "
        << "font-size=\"12\" fill=\"#2563eb\">Median simulated return</text>\n";
    svg << "  <rect x=\"" << (left + 521.0) << "\" y=\"30\" width=\"26\" height=\"12\" "
        << "fill=\"#2563eb\" fill-opacity=\"0.28\"/>\n";
    svg << "  <text x=\"" << (left + 554.0) << "\" y=\"40\" font-family=\"Arial, sans-serif\" "
        << "font-size=\"12\" fill=\"#2563eb\">P25–P75</text>\n";
    svg << "  <rect x=\"" << (left + 665.0) << "\" y=\"30\" width=\"26\" height=\"12\" "
        << "fill=\"#2563eb\" fill-opacity=\"0.12\"/>\n";
    svg << "  <text x=\"" << (left + 698.0) << "\" y=\"40\" font-family=\"Arial, sans-serif\" "
        << "font-size=\"12\" fill=\"#2563eb\">P5–P95</text>\n";

    const std::vector<double> xTicks{0.0, 25.0, 50.0, 75.0, 100.0};
    for (const double value : xTicks) {
        const double x = xFor(value);
        svg << "  <line x1=\"" << x << "\" y1=\"" << (top + plotHeight)
            << "\" x2=\"" << x << "\" y2=\"" << (top + plotHeight + 5.0)
            << "\" stroke=\"#94a3b8\" stroke-width=\"1\"/>\n";
        svg << "  <text x=\"" << x << "\" y=\"" << (top + plotHeight + 24.0)
            << "\" text-anchor=\"middle\" font-family=\"Arial, sans-serif\" "
            << "font-size=\"12\" fill=\"#64748b\">"
            << formatNumber(value, 0) << "%</text>\n";
    }

    svg << "  <text x=\"" << (left + plotWidth / 2.0)
        << "\" y=\"" << (top + plotHeight + 48.0)
        << "\" text-anchor=\"middle\" font-family=\"Arial, sans-serif\" "
        << "font-size=\"12\" fill=\"#64748b\">"
        << "Trade sequence progress"
        << "</text>\n";
    svg << "</svg>\n";

    return svg.str();
}

std::string formatSensitivityValue(double value)
{
    if (!std::isfinite(value)) {
        return "N/A";
    }

    if (std::abs(value - std::round(value)) < 1e-9) {
        return formatNumber(value, 0);
    }

    std::string text = formatNumber(value, 4);
    while (!text.empty() && text.back() == '0') {
        text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
        text.pop_back();
    }
    return text;
}

std::string makeSvgParameterSensitivityBarChart(
    const ParameterSensitivityReport& report,
    bool drawdownChart
)
{
    constexpr double width = 1000.0;
    constexpr double height = 390.0;
    constexpr double left = 72.0;
    constexpr double right = 28.0;
    constexpr double top = 34.0;
    constexpr double bottom = 92.0;
    constexpr int gridLineCount = 5;

    if (report.points.empty()) {
        return makeUnavailableSvg("No sensitivity results available");
    }

    const double plotWidth = width - left - right;
    const double plotHeight = height - top - bottom;

    double minimumValue = 0.0;
    double maximumValue = 0.0;
    for (const ParameterSensitivityPoint& point : report.points) {
        const double value = drawdownChart
            ? point.medianMaxDrawdownPercent
            : point.medianFinalReturnPercent;
        minimumValue = std::min(minimumValue, value);
        maximumValue = std::max(maximumValue, value);
    }

    if (drawdownChart) {
        minimumValue = 0.0;
        maximumValue = std::max(maximumValue * 1.10, 1.0);
    } else {
        const double span = std::max(maximumValue - minimumValue, 1.0);
        minimumValue -= span * 0.10;
        maximumValue += span * 0.10;
        minimumValue = std::min(minimumValue, 0.0);
        maximumValue = std::max(maximumValue, 0.0);
    }

    if (maximumValue - minimumValue < 1e-9) {
        maximumValue += 1.0;
        minimumValue -= drawdownChart ? 0.0 : 1.0;
    }

    const double valueRange = maximumValue - minimumValue;
    const auto yFor = [=](double value) {
        return top + ((maximumValue - value) / valueRange) * plotHeight;
    };

    const double zeroY = yFor(0.0);
    const double slotWidth = plotWidth /
        static_cast<double>(report.points.size());
    const double barWidth = std::max(2.0, slotWidth * 0.72);
    const std::string fill = drawdownChart ? "#dc2626" : "#2563eb";
    const std::string title = drawdownChart
        ? "Median maximum drawdown (%)"
        : "Median final return (%)";

    std::ostringstream svg;
    svg << "<svg viewBox=\"0 0 " << width << " " << height
        << "\" role=\"img\" aria-label=\""
        << htmlEscape(report.displayName + " " + title)
        << "\">\n";
    svg << "  <title>" << htmlEscape(report.displayName + " " + title)
        << "</title>\n";
    svg << "  <defs>\n"
        << "    <linearGradient id=\"sensitivityBarFill" << (drawdownChart ? "Dd" : "Return")
        << "\" x1=\"0\" y1=\"0\" x2=\"0\" y2=\"1\">\n"
        << "      <stop offset=\"0%\" stop-color=\"" << fill << "\" stop-opacity=\"0.88\"/>\n"
        << "      <stop offset=\"100%\" stop-color=\"" << fill << "\" stop-opacity=\"0.58\"/>\n"
        << "    </linearGradient>\n"
        << "  </defs>\n";

    for (int gridIndex = 0; gridIndex <= gridLineCount; ++gridIndex) {
        const double ratio = static_cast<double>(gridIndex) /
            static_cast<double>(gridLineCount);
        const double value = maximumValue - ratio * valueRange;
        const double y = yFor(value);

        svg << "  <line x1=\"" << left << "\" y1=\"" << y
            << "\" x2=\"" << (left + plotWidth) << "\" y2=\"" << y
            << "\" stroke=\"#e2e8f0\" stroke-width=\"1\"/>\n";
        svg << "  <text x=\"" << (left - 10.0) << "\" y=\"" << (y + 4.0)
            << "\" text-anchor=\"end\" font-family=\"Arial, sans-serif\" "
            << "font-size=\"11\" fill=\"#64748b\">"
            << htmlEscape(formatSignedPercent(value)) << "</text>\n";
    }

    for (std::size_t index = 0U; index < report.points.size(); ++index) {
        const ParameterSensitivityPoint& point = report.points[index];
        const double value = drawdownChart
            ? point.medianMaxDrawdownPercent
            : point.medianFinalReturnPercent;
        const double x = left + static_cast<double>(index) * slotWidth +
            (slotWidth - barWidth) / 2.0;
        const double valueY = yFor(value);
        const double barY = std::min(zeroY, valueY);
        const double barHeight = std::max(0.5, std::abs(valueY - zeroY));

        svg << "  <g>\n"
            << "    <title>" << htmlEscape(
                   report.displayName + " = " + formatSensitivityValue(point.parameterValue) +
                   "; " + title + " " + formatSignedPercent(value) +
                   "; positive combinations " +
                   std::to_string(point.positiveCombinationCount) + "/" +
                   std::to_string(point.totalCombinationCount)
               ) << "</title>\n"
            << "    <rect x=\"" << x << "\" y=\"" << barY
            << "\" width=\"" << barWidth << "\" height=\"" << barHeight
            << "\" rx=\"2\" fill=\"url(#sensitivityBarFill"
            << (drawdownChart ? "Dd" : "Return") << ")\"/>\n"
            << "  </g>\n";
    }

    if (!drawdownChart && minimumValue <= 0.0 && maximumValue >= 0.0) {
        svg << "  <line x1=\"" << left << "\" y1=\"" << zeroY
            << "\" x2=\"" << (left + plotWidth) << "\" y2=\"" << zeroY
            << "\" stroke=\"#ffffff\" stroke-width=\"4.5\"/>\n";
        svg << "  <line x1=\"" << left << "\" y1=\"" << zeroY
            << "\" x2=\"" << (left + plotWidth) << "\" y2=\"" << zeroY
            << "\" stroke=\"#111111\" stroke-width=\"1.8\"/>\n";
    } else {
        svg << "  <line x1=\"" << left << "\" y1=\"" << zeroY
            << "\" x2=\"" << (left + plotWidth) << "\" y2=\"" << zeroY
            << "\" stroke=\"#94a3b8\" stroke-width=\"1.2\"/>\n";
    }

    for (std::size_t index = 0U; index < report.points.size(); ++index) {
        const double x = left + (static_cast<double>(index) + 0.5) * slotWidth;
        const std::string label = formatSensitivityValue(
            report.points[index].parameterValue
        );
        const double labelY = top + plotHeight + 22.0;

        svg << "  <line x1=\"" << x << "\" y1=\"" << (top + plotHeight)
            << "\" x2=\"" << x << "\" y2=\"" << (top + plotHeight + 4.0)
            << "\" stroke=\"#94a3b8\" stroke-width=\"1\"/>\n";
        svg << "  <text x=\"" << x << "\" y=\"" << labelY
            << "\" text-anchor=\"end\" transform=\"rotate(-42 " << x
            << " " << labelY << ")\" font-family=\"Arial, sans-serif\" "
            << "font-size=\"10\" fill=\"#64748b\">"
            << htmlEscape(label) << "</text>\n";
    }

    svg << "  <text x=\"" << (left + plotWidth / 2.0)
        << "\" y=\"" << (height - 12.0)
        << "\" text-anchor=\"middle\" font-family=\"Arial, sans-serif\" "
        << "font-size=\"12\" fill=\"#64748b\">"
        << htmlEscape(report.displayName) << "</text>\n";
    svg << "</svg>\n";

    return svg.str();
}

std::string makeParameterSensitivityTable(
    const ParameterSensitivityReport& report
)
{
    std::ostringstream html;
    html << "<div class=\"sensitivity-table-wrap\">\n"
         << "<table class=\"sensitivity-table\">\n"
         << "<thead><tr>"
         << "<th>" << htmlEscape(report.displayName) << "</th>"
         << "<th>Median final return</th>"
         << "<th>Median max drawdown</th>"
         << "<th>Median trades</th>"
         << "<th>Positive combinations</th>"
         << "<th>Zero-trade combinations</th>"
         << "</tr></thead>\n<tbody>\n";

    for (const ParameterSensitivityPoint& point : report.points) {
        html << "<tr>"
             << "<td>" << htmlEscape(formatSensitivityValue(point.parameterValue)) << "</td>"
             << "<td>" << htmlEscape(formatSignedPercent(point.medianFinalReturnPercent)) << "</td>"
             << "<td>" << htmlEscape(formatPercent(point.medianMaxDrawdownPercent)) << "</td>"
             << "<td>" << htmlEscape(formatNumber(point.medianTradeCount, 1)) << "</td>"
             << "<td>" << point.positiveCombinationCount << " / "
             << point.totalCombinationCount << "</td>"
             << "<td>" << point.zeroTradeCombinationCount << "</td>"
             << "</tr>\n";
    }

    html << "</tbody></table>\n</div>\n";
    return html.str();
}

std::string makeParameterSensitivitySection(
    const std::vector<ParameterSensitivityReport>& reports
)
{
    if (reports.empty()) {
        return {};
    }

    std::ostringstream html;
    html << "  <section class=\"card\">\n"
         << "    <h2 class=\"section-title\">Parameter sensitivity</h2>\n"
         << "    <p class=\"sensitivity-intro\">Each bar summarizes the full parameter grid using the median across all valid combinations that share the displayed value. Return and drawdown are not simulated; every combination is a fresh historical backtest. Zero-trade combinations remain included and are counted in the table.</p>\n";

    for (const ParameterSensitivityReport& report : reports) {
        if (report.points.empty()) {
            continue;
        }

        html << "    <section class=\"sensitivity-parameter\">\n"
             << "      <h3 class=\"subsection-title\">"
             << htmlEscape(report.displayName) << "</h3>\n"
             << "      <div class=\"sensitivity-chart-grid\">\n"
             << "        <div class=\"sensitivity-chart\">\n"
             << "          <h4>Median final return (%)</h4>\n"
             << makeSvgParameterSensitivityBarChart(report, false)
             << "        </div>\n"
             << "        <div class=\"sensitivity-chart\">\n"
             << "          <h4>Median maximum drawdown (%)</h4>\n"
             << makeSvgParameterSensitivityBarChart(report, true)
             << "        </div>\n"
             << "      </div>\n"
             << makeParameterSensitivityTable(report)
             << "    </section>\n";
    }

    html << "  </section>\n";
    return html.str();
}

} // namespace

bool writeBacktestHtmlReport(
    const std::filesystem::path& outputPath,
    const BacktestMetrics& metrics,
    const std::vector<std::pair<Balance, Equity>>& balanceEquityHistoric,
    const MarketData& marketData,
    const std::vector<ParameterSensitivityReport>& parameterSensitivity
)
{
    std::error_code error;
    const std::filesystem::path parentPath = outputPath.parent_path();

    if (!parentPath.empty()) {
        std::filesystem::create_directories(parentPath, error);
        if (error) {
            LG_ERROR(
                "Could not create report directory '{}': {}",
                parentPath.string(),
                error.message()
            );
            return false;
        }
    }

    std::ofstream output(outputPath);
    if (!output.is_open()) {
        LG_ERROR("Could not write HTML report: {}", outputPath.string());
        return false;
    }

    const std::vector<EquityPoint> points = alignEquityCurve(
        balanceEquityHistoric,
        marketData
    );

    const std::vector<std::pair<std::string, std::string>> performanceRows{
        {"Initial equity", formatCurrency(metrics.initialEquity)},
        {"Final equity", formatCurrency(metrics.finalEquity)},
        {"Net profit", formatCurrency(metrics.netProfit)},
        {"Net return", formatPercent(metrics.netReturnPercent)},
        {"Annualized return", formatPercent(metrics.annualizedReturnPercent)},
        {"Trade count", std::to_string(metrics.tradeCount)},
        {"Exposure", formatPercent(metrics.exposurePercent)},
        {"Avg. holding time", formatNumber(metrics.averageHoldingBars, 2) + " bars"}
    };

    const std::vector<std::pair<std::string, std::string>> riskRows{
        {"Max drawdown", formatPercent(metrics.maxDrawdownPercent)},
        {"Max drawdown ($)", formatCurrency(metrics.maxDrawdownAmount)},
        {"Longest time underwater", formatDuration(
            metrics.maxDrawdownDurationBars,
            metrics.maxDrawdownDurationDays
        )},
        {"Time underwater", formatPercent(metrics.timeUnderwaterPercent)},
        {"Historical VaR (95%, 1d)", formatPercent(metrics.historicalVaR95Percent)},
        {"Historical CVaR / ES (95%, 1d)", formatPercent(metrics.historicalCvar95Percent)},
        {"Sharpe ratio", formatNumber(metrics.sharpeRatio, 3)},
        {"Sortino ratio", formatNumber(metrics.sortinoRatio, 3)},
        {"Calmar ratio", formatNumber(metrics.calmarRatio, 3)},
        {"Largest loss", formatCurrency(metrics.largestLoss)},
        {"Max consecutive losses", std::to_string(metrics.maximumConsecutiveLosses)},
        {"Worst loss streak", formatCurrency(metrics.worstConsecutiveLossPnl)}
    };

    const std::vector<std::pair<std::string, std::string>> tradeRows{
        {"Profit factor", formatNumber(metrics.profitFactor, 3)},
        {"Expectancy / trade", formatCurrency(metrics.expectancyPerTrade)},
        {"Median return / trade", formatSignedPercent(metrics.historicalMedianTradeReturnPercent)},
        {"P5 / P95 return / trade", formatSignedPercent(metrics.historicalP05TradeReturnPercent) + " / " + formatSignedPercent(metrics.historicalP95TradeReturnPercent)},
        {"Win rate", formatPercent(metrics.winRatePercent)},
        {"Average win", formatCurrency(metrics.averageWin)},
        {"Average loss", formatCurrency(metrics.averageLoss)},
        {"Gross profit", formatCurrency(metrics.grossProfit)},
        {"Gross loss", formatCurrency(-metrics.grossLoss)},
        {"Gross turnover", formatCurrency(metrics.grossTurnover)},
        {"Turnover multiple", formatNumber(metrics.turnoverMultiple, 2) + "x"}
    };

    const bool hasMonteCarlo =
        metrics.monteCarloSimulationCount > 0U;

    const auto monteCarloPercent = [hasMonteCarlo](double value) {
        return hasMonteCarlo ? formatPercent(value) : std::string("N/A");
    };

    const auto monteCarloNumber = [hasMonteCarlo](double value) {
        return hasMonteCarlo ? formatNumber(value, 1) : std::string("N/A");
    };

    const std::vector<std::pair<std::string, std::string>> probabilityRows{
        {
            "Simulations",
            hasMonteCarlo
                ? std::to_string(metrics.monteCarloSimulationCount)
                : std::string("N/A")
        },
        {
            "Trades / path",
            hasMonteCarlo
                ? std::to_string(metrics.monteCarloTradesPerSimulation)
                : std::string("N/A")
        },
        {
            "P(max DD >= 20%)",
            monteCarloPercent(
                metrics.monteCarloProbabilityDrawdown20Percent
            )
        },
        {
            "P(max DD >= 30%)",
            monteCarloPercent(
                metrics.monteCarloProbabilityDrawdown30Percent
            )
        },
        {
            "P(max DD >= 50%)",
            monteCarloPercent(
                metrics.monteCarloProbabilityDrawdown50Percent
            )
        },
        {
            "Risk of ruin (DD >= 50%)",
            monteCarloPercent(
                metrics.monteCarloRiskOfRuinPercent
            )
        }
    };

    const std::vector<std::pair<std::string, std::string>> simulatedDrawdownRows{
        {"Historical max DD", formatPercent(metrics.maxDrawdownPercent)},
        {
            "Median simulated max DD",
            monteCarloPercent(
                metrics.monteCarloMedianMaxDrawdownPercent
            )
        },
        {
            "P95 simulated max DD",
            monteCarloPercent(
                metrics.monteCarloP95MaxDrawdownPercent
            )
        },
        {
            "P99 simulated max DD",
            monteCarloPercent(
                metrics.monteCarloP99MaxDrawdownPercent
            )
        }
    };

    const std::vector<std::pair<std::string, std::string>> simulatedStreakRows{
        {
            "Historical max loss streak",
            std::to_string(metrics.maximumConsecutiveLosses)
        },
        {
            "Median max loss streak",
            monteCarloNumber(
                metrics.monteCarloMedianMaxConsecutiveLosses
            )
        },
        {
            "P95 max loss streak",
            monteCarloNumber(
                metrics.monteCarloP95MaxConsecutiveLosses
            )
        },
        {
            "P99 max loss streak",
            monteCarloNumber(
                metrics.monteCarloP99MaxConsecutiveLosses
            )
        }
    };

    output << "<!DOCTYPE html>\n"
           << "<html lang=\"en\">\n"
           << "<head>\n"
           << "  <meta charset=\"utf-8\">\n"
           << "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
           << "  <title>" << htmlEscape(metrics.strategyName) << " - Backtest report</title>\n"
           << R"HTML(
  <style>
    :root {
      color-scheme: light;
      --ink: #0f172a;
      --muted: #64748b;
      --line: #e2e8f0;
      --surface: #ffffff;
      --page: #f8fafc;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      color: var(--ink);
      background: var(--page);
      font-family: Inter, ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
    }
    main { max-width: 1180px; margin: 0 auto; padding: 34px 24px 60px; }
    .header { margin-bottom: 18px; }
    h1 { margin: 0; font-size: clamp(28px, 4vw, 42px); letter-spacing: -0.04em; }
    .card {
      background: var(--surface);
      border: 1px solid var(--line);
      border-radius: 14px;
      box-shadow: 0 1px 2px rgba(15, 23, 42, 0.04);
      overflow: hidden;
      margin-top: 18px;
    }
    .section-title {
      margin: 0;
      padding: 15px 20px;
      font-size: 17px;
      border-bottom: 1px solid var(--line);
    }
    .metrics-grid {
      display: grid;
      grid-template-columns: repeat(3, minmax(0, 1fr));
      gap: 0;
    }
    .metric-column { min-width: 0; }
    .metric-column + .metric-column { border-left: 1px solid var(--line); }
    .metric-column h2 {
      margin: 0;
      padding: 12px 16px 10px;
      font-size: 12px;
      letter-spacing: 0.06em;
      text-transform: uppercase;
      color: var(--muted);
      background: #fbfdff;
      border-bottom: 1px solid var(--line);
    }
    .metric-row {
      display: flex;
      justify-content: space-between;
      align-items: baseline;
      gap: 16px;
      min-height: 35px;
      padding: 8px 16px;
      border-bottom: 1px solid var(--line);
    }
    .metric-row:last-child { border-bottom: 0; }
    .metric-label { color: var(--ink); font-size: 13px; }
    .metric-value {
      color: var(--ink);
      font-size: 13px;
      font-weight: 700;
      text-align: right;
      white-space: nowrap;
      font-variant-numeric: tabular-nums;
    }
    .chart-wrap { padding: 14px 20px 20px; }
    .chart-wrap svg { display: block; width: 100%; height: auto; }
    .subsection-title {
      margin: 0;
      padding: 14px 20px 10px;
      font-size: 14px;
      font-weight: 700;
      border-top: 1px solid var(--line);
    }
    .sensitivity-intro {
      margin: 0;
      padding: 14px 20px;
      color: var(--muted);
      font-size: 12px;
      line-height: 1.5;
      border-bottom: 1px solid var(--line);
    }
    .sensitivity-parameter + .sensitivity-parameter { border-top: 1px solid var(--line); }
    .sensitivity-chart-grid {
      display: grid;
      grid-template-columns: repeat(2, minmax(0, 1fr));
      gap: 0;
    }
    .sensitivity-chart { min-width: 0; padding: 14px 14px 6px; }
    .sensitivity-chart + .sensitivity-chart { border-left: 1px solid var(--line); }
    .sensitivity-chart h4 {
      margin: 0 0 8px;
      font-size: 13px;
      color: var(--ink);
    }
    .sensitivity-chart svg { display: block; width: 100%; height: auto; }
    .sensitivity-table-wrap { overflow-x: auto; padding: 0 20px 20px; }
    .sensitivity-table { width: 100%; border-collapse: collapse; font-size: 12px; }
    .sensitivity-table th, .sensitivity-table td {
      padding: 9px 10px;
      border: 1px solid var(--line);
      text-align: right;
      white-space: nowrap;
      font-variant-numeric: tabular-nums;
    }
    .sensitivity-table th {
      background: #fbfdff;
      color: var(--muted);
      font-size: 11px;
      letter-spacing: 0.03em;
      text-transform: uppercase;
    }
    .sensitivity-table th:first-child, .sensitivity-table td:first-child { text-align: left; }
    .sensitivity-table tbody tr:nth-child(even) { background: #fbfdff; }
    .footnote { color: var(--muted); font-size: 12px; margin: 14px 2px 0; }
    @media (max-width: 900px) {
      .metrics-grid { grid-template-columns: 1fr; }
      .metric-column + .metric-column { border-left: 0; border-top: 1px solid var(--line); }
    }
    @media (max-width: 720px) {
      main { padding: 24px 12px 42px; }
      .metric-row { padding: 8px 12px; }
      .metric-column h2 { padding-left: 12px; padding-right: 12px; }
      .chart-wrap { padding: 10px 10px 14px; }
    }
  </style>
)HTML"
           << "</head>\n<body>\n<main>\n"
           << "  <header class=\"header\">\n"
           << "    <h1>" << htmlEscape(metrics.strategyName) << "</h1>\n"
           << "  </header>\n"
           << "  <section class=\"card\">\n"
           << "    <h2 class=\"section-title\">Backtest metrics</h2>\n"
           << "    <div class=\"metrics-grid\">\n"
           << makeMetricColumn("Performance", performanceRows)
           << makeMetricColumn("Risk", riskRows)
           << makeMetricColumn("Trade quality", tradeRows)
           << "    </div>\n"
           << "  </section>\n"
           << "  <section class=\"card\">\n"
           << "    <h2 class=\"section-title\">Equity curve</h2>\n"
           << "    <div class=\"chart-wrap\">\n"
           << makeSvgEquityCurve(points, metrics.initialEquity)
           << "    </div>\n"
           << "  </section>\n"
           << "  <section class=\"card\">\n"
           << "    <h2 class=\"section-title\">Drawdown</h2>\n"
           << "    <div class=\"chart-wrap\">\n"
           << makeSvgDrawdownCurve(points, metrics.initialEquity)
           << "    </div>\n"
           << "  </section>\n"
           << "  <section class=\"card\">\n"
           << "    <h2 class=\"section-title\">Historical net return per trade distribution</h2>\n"
           << "    <div class=\"chart-wrap\">\n"
           << makeSvgHistoricalTradeReturnHistogram(metrics)
           << "    </div>\n"
           << "  </section>\n"
           << "  <section class=\"card\">\n"
           << "    <h2 class=\"section-title\">Simulated risk scenarios</h2>\n"
           << "    <div class=\"metrics-grid\">\n"
           << makeMetricColumn("Drawdown probabilities", probabilityRows)
           << makeMetricColumn("Simulated max drawdown", simulatedDrawdownRows)
           << makeMetricColumn("Simulated loss streak", simulatedStreakRows)
           << "    </div>\n"
           << "    <h3 class=\"subsection-title\">Simulated maximum drawdown distribution</h3>\n"
           << "    <div class=\"chart-wrap\">\n"
           << makeSvgMonteCarloDrawdownHistogram(metrics)
           << "    </div>\n"
           << "    <h3 class=\"subsection-title\">Monte Carlo return fan</h3>\n"
           << "    <div class=\"chart-wrap\">\n"
           << makeSvgMonteCarloReturnFan(metrics)
           << "    </div>\n"
           << "  </section>\n"
           << makeParameterSensitivitySection(parameterSensitivity)
           << "  <p class=\"footnote\">Equity is shown as percentage change from initial equity. Drawdown is measured from the prior equity peak. Historical VaR and CVaR/Expected Shortfall use daily equity returns. Simulated risk scenarios use bootstrap paths of closed non-simulated trade PnL values, with the same number of trades per path. Risk of ruin is defined as simulated maximum drawdown of at least 50%. In the simulation, equity reaching zero or below is capped at a 100% drawdown. Historical trade returns are net PnL divided by absolute entry notional. The fan chart overlays the historical closed-trade return path, not the mark-to-market equity curve. This first model does not preserve overlapping positions or market-regime clustering.</p>\n"
           << "</main>\n</body>\n</html>\n";

    if (!output.good()) {
        LG_ERROR("Failed while writing HTML report: {}", outputPath.string());
        return false;
    }

    LG_INFO("Wrote static HTML report: {}", outputPath.string());
    return true;
}
