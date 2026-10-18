#include "scenario_reports.h"
#include "logger.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace scenario_reports {

bool ensureDirectoryExists(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::create_directories(path, error);

    if (error) {
        LG_ERROR(
            "Could not create output directory '{}': {}",
            path.string(),
            error.message()
        );
        return false;
    }

    return true;
}

std::string readTextFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        throw std::runtime_error(
            "Could not read generated chart HTML: " + path.string()
        );
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string base64Encode(const std::string& input)
{
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";

    std::string encoded;
    encoded.reserve(((input.size() + 2U) / 3U) * 4U);

    std::size_t index = 0U;

    while (index + 3U <= input.size()) {
        const unsigned int first =
            static_cast<unsigned char>(input[index]);
        const unsigned int second =
            static_cast<unsigned char>(input[index + 1U]);
        const unsigned int third =
            static_cast<unsigned char>(input[index + 2U]);

        const unsigned int value =
            (first << 16U) |
            (second << 8U) |
            third;

        encoded += alphabet[(value >> 18U) & 0x3FU];
        encoded += alphabet[(value >> 12U) & 0x3FU];
        encoded += alphabet[(value >> 6U) & 0x3FU];
        encoded += alphabet[value & 0x3FU];

        index += 3U;
    }

    const std::size_t remaining = input.size() - index;

    if (remaining == 1U) {
        const unsigned int first =
            static_cast<unsigned char>(input[index]);

        encoded += alphabet[(first >> 2U) & 0x3FU];
        encoded += alphabet[(first << 4U) & 0x3FU];
        encoded += '=';
        encoded += '=';
    } else if (remaining == 2U) {
        const unsigned int first =
            static_cast<unsigned char>(input[index]);
        const unsigned int second =
            static_cast<unsigned char>(input[index + 1U]);

        encoded += alphabet[(first >> 2U) & 0x3FU];
        encoded += alphabet[
            ((first << 4U) | (second >> 4U)) & 0x3FU
        ];
        encoded += alphabet[(second << 2U) & 0x3FU];
        encoded += '=';
    }

    return encoded;
}

void writeStrategyIndexHtml(
    const std::filesystem::path& outputPath,
    const std::string& strategyName,
    const std::vector<ChartEntry>& charts
)
{
    std::ofstream output(outputPath);
    if (!output.is_open()) {
        throw std::runtime_error(
            "Could not write strategy index HTML: " + outputPath.string()
        );
    }

    output <<
        "<!doctype html>\n"
        "<html lang=\"en\">\n"
        "<head>\n"
        "  <meta charset=\"utf-8\">\n"
        "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
        "  <title>" << strategyName << " - all datasets</title>\n"
        "  <style>\n"
        "    :root { color-scheme: light dark; }\n"
        "    body { margin: 0; font-family: Arial, sans-serif; background: #111827; "
        "color: #e5e7eb; }\n"
        "    main { width: min(1500px, calc(100% - 32px)); margin: 24px auto 64px; }\n"
        "    h1 { margin-bottom: 8px; }\n"
        "    .summary { color: #9ca3af; margin-top: 0; }\n"
        "    section { margin: 28px 0; padding: 18px; border: 1px solid #374151; "
        "border-radius: 12px; background: #1f2937; }\n"
        "    h2 { margin: 0 0 6px; }\n"
        "    .meta { margin: 0 0 14px; color: #9ca3af; }\n"
        "    iframe { width: 100%; height: 760px; border: 0; border-radius: 8px; "
        "background: white; }\n"
        "    a { color: #93c5fd; }\n"
        "  </style>\n"
        "</head>\n"
        "<body>\n"
        "<main>\n"
        "  <h1>" << strategyName << "</h1>\n"
        "  <p class=\"summary\">" << charts.size()
        << " isolated dataset backtests. Each chart uses a fresh strategy, "
        "balance, equity, trade ID and BacktestContext.</p>\n";

    for (const ChartEntry& chart : charts) {
        output <<
            "  <section>\n"
            "    <h2>" << chart.datasetId << "</h2>\n"
            "    <p class=\"meta\">Dataset: " << chart.datasetFileName
            << " &middot; benchmark: " << chart.benchmarkSymbol
            << " (buy &amp; hold equity from OHLCV close, starting at 100,000)"
            << "</p>\n"
            "    <iframe loading=\"lazy\" srcdoc=\""
            << escapeHtmlAttribute(chart.chartHtml)
            << "\" title=\"" << strategyName << " - " << chart.datasetId
            << "\"></iframe>\n"
            "  </section>\n";
    }

    output <<
        "</main>\n"
        "</body>\n"
        "</html>\n";
}



void write1dCmcCorrelationHtml(
    const std::filesystem::path& outputPath,
    const std::vector<CorrelationChartEntry>& strategyCharts,
    const std::vector<BenchmarkPoint>& btcPoints,
    const std::string& btcSymbol,
    const double initialValue
)
{
    if (strategyCharts.empty()) {
        throw std::runtime_error(
            "Cannot create the 1d_cmc correlation report without strategy curves."
        );
    }

    if (btcPoints.empty()) {
        throw std::runtime_error(
            "Cannot create the 1d_cmc correlation report without BTC data."
        );
    }

    const double firstClose = btcPoints.front().close;
    if (!std::isfinite(firstClose) || firstClose <= 0.0) {
        throw std::runtime_error(
            "Cannot create the BTC buy-and-hold curve because its first close "
            "is invalid."
        );
    }

    std::ofstream output(outputPath);
    if (!output.is_open()) {
        throw std::runtime_error(
            "Could not write 1d_cmc correlation HTML: " + outputPath.string()
        );
    }

    output << std::setprecision(17);

    output <<
R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>1d_cmc strategy equity and correlations</title>
  <style>
    :root {
      color-scheme: light dark;
      --background: #111827;
      --panel: #1f2937;
      --panel-soft: #182234;
      --border: #374151;
      --text: #e5e7eb;
      --muted: #9ca3af;
      --accent: #60a5fa;
    }

    * { box-sizing: border-box; }

    body {
      margin: 0;
      font-family: Arial, sans-serif;
      background: var(--background);
      color: var(--text);
    }

    main {
      width: min(1600px, calc(100% - 32px));
      margin: 24px auto 64px;
    }

    h1, h2 { margin-top: 0; }
    h1 { margin-bottom: 8px; }
    h2 { margin-bottom: 10px; }

    .intro,
    .note,
    .status {
      color: var(--muted);
    }

    .status {
      margin: 12px 0 0;
      padding: 10px 12px;
      border: 1px solid var(--border);
      border-radius: 8px;
      background: var(--panel-soft);
    }

    .status.error {
      color: #fecaca;
      border-color: #991b1b;
      background: #450a0a;
      white-space: pre-wrap;
    }

    .panel {
      margin-top: 24px;
      padding: 18px;
      border: 1px solid var(--border);
      border-radius: 12px;
      background: var(--panel);
    }

    .toolbar {
      display: flex;
      flex-wrap: wrap;
      align-items: end;
      gap: 12px;
      margin-bottom: 14px;
    }

    label {
      display: grid;
      gap: 5px;
      color: var(--muted);
      font-size: 14px;
    }

    select,
    button {
      border: 1px solid #4b5563;
      border-radius: 8px;
      background: var(--panel-soft);
      color: var(--text);
      padding: 8px 11px;
      font: inherit;
    }

    button { cursor: pointer; }
    button:hover { border-color: var(--accent); }

    .canvas-wrap {
      width: 100%;
      min-height: 560px;
      border-radius: 9px;
      overflow: hidden;
      background: #ffffff;
    }

    canvas {
      display: block;
      width: 100%;
      height: 560px;
    }

    .legend {
      display: flex;
      flex-wrap: wrap;
      gap: 8px 16px;
      margin-top: 12px;
    }

    .legend-item {
      display: inline-flex;
      align-items: center;
      gap: 7px;
      color: var(--text);
      font-size: 14px;
    }

    .legend-line {
      display: inline-block;
      width: 24px;
      height: 3px;
      border-radius: 999px;
    }

    .table-wrap {
      width: 100%;
      overflow-x: auto;
    }

    table {
      width: 100%;
      border-collapse: collapse;
      font-variant-numeric: tabular-nums;
    }

    th,
    td {
      padding: 10px 12px;
      border: 1px solid var(--border);
      text-align: right;
      white-space: nowrap;
    }

    th:first-child,
    td:first-child {
      position: sticky;
      left: 0;
      text-align: left;
      background: var(--panel);
      z-index: 1;
    }

    thead th {
      background: var(--panel-soft);
    }

    .corr-value {
      display: block;
      font-weight: 600;
    }

    .corr-count {
      display: block;
      margin-top: 2px;
      font-size: 11px;
      opacity: 0.8;
    }

    @media (max-width: 700px) {
      main { width: min(100% - 18px, 1600px); }
      .panel { padding: 12px; }
      canvas { height: 430px; }
      .canvas-wrap { min-height: 430px; }
    }
  </style>
</head>
<body>
<main>
  <h1>1d_cmc strategy equity and correlations</h1>
  <p class="intro">
    Enabled strategy equity curves from the isolated 1d_cmc backtests,
    together with BTC buy &amp; hold starting at 100,000.
  </p>
  <p id="status" class="status">Preparing embedded strategy curves…</p>

  <section class="panel">
    <h2>Combined equity curves</h2>
    <div class="toolbar">
      <label>
        Y-axis
        <select id="scale-select">
          <option value="linear">Linear</option>
          <option value="log">Logarithmic</option>
        </select>
      </label>
      <button id="download-equity" type="button">Download equity CSV</button>
      <button id="download-correlation" type="button">
        Download correlation CSV
      </button>
    </div>

    <div class="canvas-wrap">
      <canvas id="equity-canvas" aria-label="Combined strategy equity curves"></canvas>
    </div>
    <div id="legend" class="legend"></div>
    <p class="note">
      Every curve is shown in account-value units. Each isolated strategy and
      BTC buy &amp; hold starts at 100,000.
    </p>
  </section>

  <section class="panel">
    <h2>Curve summary</h2>
    <div class="table-wrap">
      <table id="summary-table"></table>
    </div>
  </section>

  <section class="panel">
    <h2>Daily-return correlation matrix</h2>
    <p class="note">
      Pearson correlation is calculated from daily arithmetic returns using
      the pairwise intersection of available dates.
    </p>
    <div class="table-wrap">
      <table id="correlation-table"></table>
    </div>
  </section>
</main>

<script>
(() => {
  "use strict";

  const embeddedStrategies = [
)HTML";

    for (std::size_t index = 0U; index < strategyCharts.size(); ++index) {
        if (index != 0U) {
            output << ",\n";
        }

        output <<
            "    {\"name\":\""
            << escapeJavaScriptString(strategyCharts[index].strategyName)
            << "\",\"datesBase64\":\""
            << base64Encode(strategyCharts[index].datesArrayExpression)
            << "\",\"equityBase64\":\""
            << base64Encode(strategyCharts[index].equityArrayExpression)
            << "\"}";
    }

    output <<
R"HTML(
  ];

  const embeddedBtc = {
    name: ")HTML"
           << escapeJavaScriptString(btcSymbol)
           << R"HTML( Buy & Hold",
    x: [
)HTML";

    for (std::size_t index = 0U; index < btcPoints.size(); ++index) {
        if (index != 0U) {
            output << ",\n";
        }

        output << "      \""
               << escapeJavaScriptString(btcPoints[index].timestamp)
               << "\"";
    }

    output <<
R"HTML(
    ],
    y: [
)HTML";

    for (std::size_t index = 0U; index < btcPoints.size(); ++index) {
        if (index != 0U) {
            output << ",\n";
        }

        const double equity =
            initialValue * btcPoints[index].close / firstClose;

        output << "      " << equity;
    }

    output <<
R"HTML(
    ]
  };

  const palette = [
    "#2563eb",
    "#dc2626",
    "#7c3aed",
    "#ea580c",
    "#0891b2",
    "#be185d",
    "#4d7c0f",
    "#0f766e",
    "#a16207",
    "#475569",
    "#16a34a"
  ];

  const state = {
    series: [],
    correlations: [],
    scale: "linear"
  };

  const statusElement = document.getElementById("status");
  const canvas = document.getElementById("equity-canvas");
  const context = canvas.getContext("2d");
  const legendElement = document.getElementById("legend");
  const summaryTable = document.getElementById("summary-table");
  const correlationTable = document.getElementById("correlation-table");
  const scaleSelect = document.getElementById("scale-select");

  function decodeBase64Utf8(encoded) {
    const binary = window.atob(encoded);
    const bytes = new Uint8Array(binary.length);

    for (let index = 0; index < binary.length; ++index) {
      bytes[index] = binary.charCodeAt(index);
    }

    return new TextDecoder("utf-8").decode(bytes);
  }

  function evaluateSavedArray(
    encoded,
    arrayDescription,
    strategyName
  ) {
    const expression = decodeBase64Utf8(encoded);
    let values = null;

    try {
      values = JSON.parse(expression);
    } catch (jsonError) {
      try {
        const evaluator = new Function(
          "\"use strict\"; return (" + expression + ");"
        );
        values = evaluator();
      } catch (evaluationError) {
        throw new Error(
          "Could not evaluate the saved " + arrayDescription +
          " array for " + strategyName + ": " +
          evaluationError.message
        );
      }
    }

    if (!Array.isArray(values)) {
      throw new Error(
        "The saved " + arrayDescription + " data for " +
        strategyName + " did not evaluate to an array."
      );
    }

    return values;
  }

  function normalizedDate(value) {
    if (value instanceof Date && Number.isFinite(value.getTime())) {
      return value.toISOString().slice(0, 10);
    }

    if (typeof value === "number" && Number.isFinite(value)) {
      const integerText = String(Math.trunc(value));

      if (/^\d{8}$/.test(integerText)) {
        return (
          integerText.slice(0, 4) + "-" +
          integerText.slice(4, 6) + "-" +
          integerText.slice(6, 8)
        );
      }

      const milliseconds =
        Math.abs(value) < 100000000000 ? value * 1000 : value;
      const date = new Date(milliseconds);

      return Number.isFinite(date.getTime())
        ? date.toISOString().slice(0, 10)
        : "";
    }

    const text = String(value ?? "").trim();
    if (!text) {
      return "";
    }

    if (/^\d{8}(?:\.0)?$/.test(text)) {
      const digits = text.slice(0, 8);
      return (
        digits.slice(0, 4) + "-" +
        digits.slice(4, 6) + "-" +
        digits.slice(6, 8)
      );
    }

    if (/^\d{10}$/.test(text) || /^\d{13}$/.test(text)) {
      const numeric = Number(text);
      const milliseconds = text.length === 10
        ? numeric * 1000
        : numeric;
      const date = new Date(milliseconds);

      return Number.isFinite(date.getTime())
        ? date.toISOString().slice(0, 10)
        : "";
    }

    const date = new Date(text.replace(" ", "T"));
    if (Number.isFinite(date.getTime())) {
      return date.toISOString().slice(0, 10);
    }

    return text.slice(0, 10);
  }

  function cleanSeries(name, xValues, yValues) {
    const valuesByDate = new Map();
    const count = Math.min(xValues.length, yValues.length);

    for (let index = 0; index < count; ++index) {
      const date = normalizedDate(xValues[index]);
      const value = Number(yValues[index]);

      if (date && Number.isFinite(value) && value > 0) {
        valuesByDate.set(date, value);
      }
    }

    const dates = Array.from(valuesByDate.keys()).sort();
    const values = dates.map((date) => valuesByDate.get(date));

    return {
      name,
      dates,
      values,
      color: palette[state.series.length % palette.length]
    };
  }

  function extractStrategySeries(source) {
    const dates = evaluateSavedArray(
      source.datesBase64,
      "dates",
      source.name
    );

    const equityValues = evaluateSavedArray(
      source.equityBase64,
      "equity",
      source.name
    );

    const series = cleanSeries(
      source.name,
      dates,
      equityValues
    );

    if (series.dates.length < 2) {
      throw new Error(
        "The saved Equity curve for strategy " + source.name +
        " contains fewer than two valid points."
      );
    }

    return series;
  }

  function returnsByDate(series) {
    const returns = new Map();

    for (let index = 1; index < series.values.length; ++index) {
      const previous = series.values[index - 1];
      const current = series.values[index];

      if (
        Number.isFinite(previous) &&
        Number.isFinite(current) &&
        previous > 0
      ) {
        returns.set(
          series.dates[index],
          current / previous - 1
        );
      }
    }

    return returns;
  }

  function pearsonCorrelation(leftSeries, rightSeries) {
    const leftReturns = returnsByDate(leftSeries);
    const rightReturns = returnsByDate(rightSeries);
    const leftValues = [];
    const rightValues = [];

    for (const [date, leftValue] of leftReturns.entries()) {
      if (!rightReturns.has(date)) {
        continue;
      }

      const rightValue = rightReturns.get(date);

      if (Number.isFinite(leftValue) && Number.isFinite(rightValue)) {
        leftValues.push(leftValue);
        rightValues.push(rightValue);
      }
    }

    const observations = leftValues.length;

    if (leftSeries === rightSeries) {
      return {
        value: observations > 0 ? 1 : Number.NaN,
        observations
      };
    }

    if (observations < 2) {
      return {
        value: Number.NaN,
        observations
      };
    }

    const leftMean =
      leftValues.reduce((sum, value) => sum + value, 0) / observations;
    const rightMean =
      rightValues.reduce((sum, value) => sum + value, 0) / observations;

    let covariance = 0;
    let leftVariance = 0;
    let rightVariance = 0;

    for (let index = 0; index < observations; ++index) {
      const leftCentered = leftValues[index] - leftMean;
      const rightCentered = rightValues[index] - rightMean;

      covariance += leftCentered * rightCentered;
      leftVariance += leftCentered * leftCentered;
      rightVariance += rightCentered * rightCentered;
    }

    const denominator = Math.sqrt(leftVariance * rightVariance);

    return {
      value: denominator > 0
        ? covariance / denominator
        : Number.NaN,
      observations
    };
  }

  function calculateCorrelations() {
    state.correlations = state.series.map((leftSeries) => {
      return state.series.map((rightSeries) => {
        return pearsonCorrelation(leftSeries, rightSeries);
      });
    });
  }

  function compactNumber(value) {
    if (!Number.isFinite(value)) {
      return "—";
    }

    const absolute = Math.abs(value);

    if (absolute >= 1e9) {
      return (value / 1e9).toFixed(2) + "B";
    }
    if (absolute >= 1e6) {
      return (value / 1e6).toFixed(2) + "M";
    }
    if (absolute >= 1e3) {
      return (value / 1e3).toFixed(1) + "K";
    }

    return value.toFixed(0);
  }

  function formatFullNumber(value) {
    return Number.isFinite(value)
      ? value.toLocaleString(undefined, {
          maximumFractionDigits: 2
        })
      : "—";
  }

  function formatPercent(value) {
    return Number.isFinite(value)
      ? (value * 100).toFixed(2) + "%"
      : "—";
  }

  function correlationCellColor(value) {
    if (!Number.isFinite(value)) {
      return "transparent";
    }

    const strength = Math.min(1, Math.abs(value));
    const alpha = 0.13 + 0.55 * strength;

    return value >= 0
      ? `rgba(22, 163, 74, ${alpha})`
      : `rgba(220, 38, 38, ${alpha})`;
  }

  function renderLegend() {
    legendElement.innerHTML = "";

    for (const series of state.series) {
      const item = document.createElement("span");
      item.className = "legend-item";

      const line = document.createElement("span");
      line.className = "legend-line";
      line.style.backgroundColor = series.color;

      const label = document.createElement("span");
      label.textContent = series.name;

      item.appendChild(line);
      item.appendChild(label);
      legendElement.appendChild(item);
    }
  }

  function renderSummaryTable() {
    summaryTable.innerHTML = "";

    const header = document.createElement("thead");
    const headerRow = document.createElement("tr");

    for (const label of [
      "Curve",
      "Start",
      "End",
      "Points",
      "Initial equity",
      "Final equity",
      "Total return"
    ]) {
      const cell = document.createElement("th");
      cell.textContent = label;
      headerRow.appendChild(cell);
    }

    header.appendChild(headerRow);
    summaryTable.appendChild(header);

    const body = document.createElement("tbody");

    for (const series of state.series) {
      const row = document.createElement("tr");
      const first = series.values[0];
      const last = series.values[series.values.length - 1];
      const values = [
        series.name,
        series.dates[0],
        series.dates[series.dates.length - 1],
        String(series.values.length),
        formatFullNumber(first),
        formatFullNumber(last),
        first > 0 ? formatPercent(last / first - 1) : "—"
      ];

      values.forEach((value, index) => {
        const cell = document.createElement(index === 0 ? "th" : "td");
        cell.textContent = value;
        row.appendChild(cell);
      });

      body.appendChild(row);
    }

    summaryTable.appendChild(body);
  }

  function renderCorrelationTable() {
    correlationTable.innerHTML = "";

    const header = document.createElement("thead");
    const headerRow = document.createElement("tr");
    const emptyCorner = document.createElement("th");
    emptyCorner.textContent = "Daily returns";
    headerRow.appendChild(emptyCorner);

    for (const series of state.series) {
      const cell = document.createElement("th");
      cell.textContent = series.name;
      headerRow.appendChild(cell);
    }

    header.appendChild(headerRow);
    correlationTable.appendChild(header);

    const body = document.createElement("tbody");

    state.series.forEach((series, rowIndex) => {
      const row = document.createElement("tr");
      const rowHeader = document.createElement("th");
      rowHeader.textContent = series.name;
      row.appendChild(rowHeader);

      state.series.forEach((_, columnIndex) => {
        const result = state.correlations[rowIndex][columnIndex];
        const cell = document.createElement("td");

        cell.style.backgroundColor = correlationCellColor(result.value);

        const value = document.createElement("span");
        value.className = "corr-value";
        value.textContent = Number.isFinite(result.value)
          ? result.value.toFixed(3)
          : "—";

        const count = document.createElement("span");
        count.className = "corr-count";
        count.textContent = "n=" + result.observations;

        cell.appendChild(value);
        cell.appendChild(count);
        row.appendChild(cell);
      });

      body.appendChild(row);
    });

    correlationTable.appendChild(body);
  }

  function globalTimeRange() {
    let minimum = Number.POSITIVE_INFINITY;
    let maximum = Number.NEGATIVE_INFINITY;

    for (const series of state.series) {
      for (const date of series.dates) {
        const milliseconds = Date.parse(date + "T00:00:00Z");

        if (Number.isFinite(milliseconds)) {
          minimum = Math.min(minimum, milliseconds);
          maximum = Math.max(maximum, milliseconds);
        }
      }
    }

    return { minimum, maximum };
  }

  function transformY(value) {
    if (state.scale === "log") {
      return value > 0 ? Math.log10(value) : Number.NaN;
    }

    return value;
  }

  function drawEquityChart() {
    const rectangle = canvas.getBoundingClientRect();
    const deviceRatio = window.devicePixelRatio || 1;
    const cssWidth = Math.max(320, rectangle.width);
    const cssHeight = window.innerWidth <= 700 ? 430 : 560;

    canvas.width = Math.round(cssWidth * deviceRatio);
    canvas.height = Math.round(cssHeight * deviceRatio);
    canvas.style.height = cssHeight + "px";

    context.setTransform(deviceRatio, 0, 0, deviceRatio, 0, 0);
    context.clearRect(0, 0, cssWidth, cssHeight);
    context.fillStyle = "#ffffff";
    context.fillRect(0, 0, cssWidth, cssHeight);

    if (state.series.length === 0) {
      return;
    }

    const margins = {
      left: 82,
      right: 24,
      top: 22,
      bottom: 54
    };

    const plotWidth = cssWidth - margins.left - margins.right;
    const plotHeight = cssHeight - margins.top - margins.bottom;
    const timeRange = globalTimeRange();

    if (
      !Number.isFinite(timeRange.minimum) ||
      !Number.isFinite(timeRange.maximum) ||
      timeRange.maximum <= timeRange.minimum
    ) {
      return;
    }

    let minimumY = Number.POSITIVE_INFINITY;
    let maximumY = Number.NEGATIVE_INFINITY;

    for (const series of state.series) {
      for (const rawValue of series.values) {
        const value = transformY(rawValue);

        if (Number.isFinite(value)) {
          minimumY = Math.min(minimumY, value);
          maximumY = Math.max(maximumY, value);
        }
      }
    }

    if (!Number.isFinite(minimumY) || !Number.isFinite(maximumY)) {
      return;
    }

    if (maximumY <= minimumY) {
      maximumY = minimumY + 1;
    }

    const yPadding = (maximumY - minimumY) * 0.06;
    minimumY -= yPadding;
    maximumY += yPadding;

    const xPosition = (milliseconds) => {
      return margins.left +
        (milliseconds - timeRange.minimum) /
        (timeRange.maximum - timeRange.minimum) *
        plotWidth;
    };

    const yPosition = (rawValue) => {
      const value = transformY(rawValue);

      return margins.top +
        (maximumY - value) /
        (maximumY - minimumY) *
        plotHeight;
    };

    context.strokeStyle = "#e5e7eb";
    context.fillStyle = "#374151";
    context.lineWidth = 1;
    context.font = "12px Arial";

    const horizontalTicks = 6;

    for (let tick = 0; tick <= horizontalTicks; ++tick) {
      const ratio = tick / horizontalTicks;
      const y = margins.top + ratio * plotHeight;
      const transformedValue = maximumY - ratio * (maximumY - minimumY);
      const rawValue = state.scale === "log"
        ? Math.pow(10, transformedValue)
        : transformedValue;

      context.beginPath();
      context.moveTo(margins.left, y);
      context.lineTo(margins.left + plotWidth, y);
      context.stroke();

      context.textAlign = "right";
      context.textBaseline = "middle";
      context.fillText(
        compactNumber(rawValue),
        margins.left - 10,
        y
      );
    }

    const verticalTicks = 7;

    for (let tick = 0; tick <= verticalTicks; ++tick) {
      const ratio = tick / verticalTicks;
      const milliseconds =
        timeRange.minimum +
        ratio * (timeRange.maximum - timeRange.minimum);
      const x = margins.left + ratio * plotWidth;
      const date = new Date(milliseconds);

      context.beginPath();
      context.moveTo(x, margins.top);
      context.lineTo(x, margins.top + plotHeight);
      context.stroke();

      context.textAlign = "center";
      context.textBaseline = "top";
      context.fillText(
        String(date.getUTCFullYear()),
        x,
        margins.top + plotHeight + 12
      );
    }

    context.save();
    context.beginPath();
    context.rect(
      margins.left,
      margins.top,
      plotWidth,
      plotHeight
    );
    context.clip();

    for (const series of state.series) {
      context.beginPath();
      context.strokeStyle = series.color;
      context.lineWidth = series.name.includes("Buy & Hold") ? 2.5 : 2;
      context.setLineDash(
        series.name.includes("Buy & Hold") ? [7, 5] : []
      );

      let started = false;

      for (let index = 0; index < series.dates.length; ++index) {
        const milliseconds = Date.parse(
          series.dates[index] + "T00:00:00Z"
        );
        const rawValue = series.values[index];
        const transformedValue = transformY(rawValue);

        if (
          !Number.isFinite(milliseconds) ||
          !Number.isFinite(transformedValue)
        ) {
          started = false;
          continue;
        }

        const x = xPosition(milliseconds);
        const y = yPosition(rawValue);

        if (!started) {
          context.moveTo(x, y);
          started = true;
        } else {
          context.lineTo(x, y);
        }
      }

      context.stroke();
    }

    context.restore();
    context.setLineDash([]);

    context.strokeStyle = "#6b7280";
    context.strokeRect(
      margins.left,
      margins.top,
      plotWidth,
      plotHeight
    );
  }

  function csvCell(value) {
    const text = String(value ?? "");

    return /[",\n\r]/.test(text)
      ? "\"" + text.replace(/"/g, "\"\"") + "\""
      : text;
  }

  function downloadText(filename, text) {
    const blob = new Blob([text], {
      type: "text/csv;charset=utf-8"
    });
    const url = URL.createObjectURL(blob);
    const anchor = document.createElement("a");

    anchor.href = url;
    anchor.download = filename;
    document.body.appendChild(anchor);
    anchor.click();
    anchor.remove();

    window.setTimeout(() => {
      URL.revokeObjectURL(url);
    }, 0);
  }

  function downloadEquityCsv() {
    const allDates = Array.from(
      new Set(
        state.series.flatMap((series) => series.dates)
      )
    ).sort();

    const maps = state.series.map((series) => {
      const map = new Map();

      series.dates.forEach((date, index) => {
        map.set(date, series.values[index]);
      });

      return map;
    });

    const rows = [
      ["Date", ...state.series.map((series) => series.name)]
    ];

    for (const date of allDates) {
      rows.push([
        date,
        ...maps.map((map) => {
          return map.has(date) ? map.get(date) : "";
        })
      ]);
    }

    const csv = rows
      .map((row) => row.map(csvCell).join(","))
      .join("\n");

    downloadText("1d_cmc_equity_curves.csv", csv);
  }

  function downloadCorrelationCsv() {
    const rows = [
      ["Daily returns", ...state.series.map((series) => series.name)]
    ];

    state.series.forEach((series, rowIndex) => {
      rows.push([
        series.name,
        ...state.correlations[rowIndex].map((result) => {
          return Number.isFinite(result.value)
            ? result.value
            : "";
        })
      ]);
    });

    const csv = rows
      .map((row) => row.map(csvCell).join(","))
      .join("\n");

    downloadText("1d_cmc_daily_return_correlations.csv", csv);
  }

  function initialize() {
    try {
      for (const source of embeddedStrategies) {
        const series = extractStrategySeries(source);
        series.color = palette[state.series.length % palette.length];
        state.series.push(series);
      }

      const btcSeries = cleanSeries(
        embeddedBtc.name,
        embeddedBtc.x,
        embeddedBtc.y
      );

      if (btcSeries.dates.length < 2) {
        throw new Error(
          "BTC Buy & Hold contains fewer than two valid points."
        );
      }

      btcSeries.color = "#16a34a";
      state.series.push(btcSeries);

      calculateCorrelations();
      renderLegend();
      renderSummaryTable();
      renderCorrelationTable();
      drawEquityChart();

      statusElement.textContent =
        "Loaded " + state.series.length + " curves: " +
        state.series.map((series) => series.name).join(", ");
    } catch (error) {
      statusElement.className = "status error";
      statusElement.textContent =
        "Could not generate the correlation report:\n" +
        (error && error.message ? error.message : String(error));
    }
  }

  scaleSelect.addEventListener("change", () => {
    state.scale = scaleSelect.value;
    drawEquityChart();
  });

  document
    .getElementById("download-equity")
    .addEventListener("click", downloadEquityCsv);

  document
    .getElementById("download-correlation")
    .addEventListener("click", downloadCorrelationCsv);

  window.addEventListener("resize", drawEquityChart);

  initialize();
})();
</script>
</body>
</html>
)HTML";
}

} // namespace scenario_reports
