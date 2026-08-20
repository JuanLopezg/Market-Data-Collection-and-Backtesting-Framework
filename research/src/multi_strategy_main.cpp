#include "backtest.h"
#include "all_strategies.h"
#include "backtest_helpers.h"
#include "benchmark_above_sma_filter.h"
#include "database_utils.h"
#include "indicator_ranker.h"
#include "indicator_spec.h"
#include "liquidity_universe.h"
#include "logger.h"
#include "market_filter.h"
#include "universe_selector.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <functional>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

// Multiple switches may be enabled simultaneously.
// Every enabled strategy is run independently against every configured dataset.
#define ENABLE_BARGAIN_CHASER 0
#define ENABLE_ATR_BREAKOUT 0
#define ENABLE_MR_SHORT 0
#define ENABLE_PURE_MOM 0
#define ENABLE_PURE_RSI 1
#define ENABLE_MR_RSI_LONG 0
#define ENABLE_XH_BREAKOUT 1
#define ENABLE_DONCHIAN_BREAKOUT 1

namespace {

struct DatasetDefinition {
    std::string id;
    std::string fileName;
    std::string benchmarkSymbol;
    unsigned int benchmarkMovingAverageLength;
};

struct StrategyDefinition {
    std::string name;
    std::string outputSlug;
    std::function<std::unique_ptr<Strategy>(
        const std::string& benchmarkSymbol,
        unsigned int benchmarkMovingAverageLength
    )> create;
};

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

std::string escapeHtmlAttribute(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size());

    for (const char character : value) {
        switch (character) {
        case '&':
            escaped += "&amp;";
            break;
        case '"':
            escaped += "&quot;";
            break;
        case '\'':
            escaped += "&#39;";
            break;
        case '<':
            escaped += "&lt;";
            break;
        case '>':
            escaped += "&gt;";
            break;
        default:
            escaped += character;
            break;
        }
    }

    return escaped;
}

struct BenchmarkPoint {
    std::string timestamp;
    double close;
};

std::string trimCopy(const std::string& value)
{
    const auto first = std::find_if_not(
        value.begin(),
        value.end(),
        [](unsigned char character) { return std::isspace(character) != 0; }
    );

    const auto last = std::find_if_not(
        value.rbegin(),
        value.rend(),
        [](unsigned char character) { return std::isspace(character) != 0; }
    ).base();

    if (first >= last) {
        return {};
    }

    return std::string(first, last);
}

std::string lowerCopy(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        }
    );
    return value;
}

char detectCsvDelimiter(const std::string& header)
{
    const std::vector<char> candidates{',', ';', '\t', '|'};
    char bestDelimiter = ',';
    std::size_t bestCount = 0U;

    for (const char candidate : candidates) {
        const std::size_t count = static_cast<std::size_t>(
            std::count(header.begin(), header.end(), candidate)
        );
        if (count > bestCount) {
            bestCount = count;
            bestDelimiter = candidate;
        }
    }

    return bestDelimiter;
}

std::vector<std::string> parseCsvRow(
    const std::string& line,
    const char delimiter
)
{
    std::vector<std::string> fields;
    std::string field;
    bool insideQuotes = false;

    for (std::size_t index = 0U; index < line.size(); ++index) {
        const char character = line[index];

        if (character == '"') {
            if (insideQuotes && index + 1U < line.size() && line[index + 1U] == '"') {
                field += '"';
                ++index;
            } else {
                insideQuotes = !insideQuotes;
            }
        } else if (character == delimiter && !insideQuotes) {
            fields.push_back(trimCopy(field));
            field.clear();
        } else {
            field += character;
        }
    }

    fields.push_back(trimCopy(field));
    return fields;
}

std::size_t findCsvColumn(
    const std::vector<std::string>& headers,
    const std::vector<std::string>& candidates
)
{
    for (std::size_t index = 0U; index < headers.size(); ++index) {
        std::string normalized = lowerCopy(trimCopy(headers[index]));
        if (index == 0U && normalized.size() >= 3U &&
            static_cast<unsigned char>(normalized[0]) == 0xEFU &&
            static_cast<unsigned char>(normalized[1]) == 0xBBU &&
            static_cast<unsigned char>(normalized[2]) == 0xBFU) {
            normalized.erase(0U, 3U);
        }

        if (std::find(candidates.begin(), candidates.end(), normalized) !=
            candidates.end()) {
            return index;
        }
    }

    return std::numeric_limits<std::size_t>::max();
}

bool benchmarkSymbolsMatch(
    const std::string& csvSymbol,
    const std::string& benchmarkSymbol
)
{
    // IMPORTANT: exact match only.
    //
    // The previous substring match made benchmark "BTC" also accept symbols
    // such as BTCUSDT, WBTC, BTCB, etc. Plotting those prices as one series
    // produced the vertical comb/zig-zag pattern.
    return lowerCopy(trimCopy(csvSymbol)) ==
        lowerCopy(trimCopy(benchmarkSymbol));
}

bool parseFiniteDouble(const std::string& text, double& result)
{
    try {
        const std::string trimmed = trimCopy(text);
        std::size_t consumed = 0U;
        result = std::stod(trimmed, &consumed);
        return consumed == trimmed.size() && std::isfinite(result);
    } catch (...) {
        return false;
    }
}

std::string formatUnixTimestampForPlotly(const long long rawTimestamp)
{
    long long milliseconds = rawTimestamp;

    // Ten digits normally means Unix seconds; 13 digits normally means
    // Unix milliseconds.
    if (std::llabs(milliseconds) < 100000000000LL) {
        milliseconds *= 1000LL;
    }

    const std::time_t seconds = static_cast<std::time_t>(milliseconds / 1000LL);
    std::tm utcTime{};

#if defined(_WIN32)
    if (gmtime_s(&utcTime, &seconds) != 0) {
        return std::to_string(rawTimestamp);
    }
#else
    if (gmtime_r(&seconds, &utcTime) == nullptr) {
        return std::to_string(rawTimestamp);
    }
#endif

    std::ostringstream formatted;
    formatted << std::put_time(&utcTime, "%Y-%m-%dT%H:%M:%S");
    return formatted.str();
}

bool isAllDigits(const std::string& value)
{
    return !value.empty() && std::all_of(
        value.begin(),
        value.end(),
        [](unsigned char character) {
            return std::isdigit(character) != 0;
        }
    );
}

bool looksLikeCalendarTimestamp(const std::string& digits)
{
    if (digits.size() < 8U || !isAllDigits(digits)) {
        return false;
    }

    const int year = std::stoi(digits.substr(0U, 4U));
    const int month = std::stoi(digits.substr(4U, 2U));
    const int day = std::stoi(digits.substr(6U, 2U));

    return year >= 1900 && year <= 2200 &&
        month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

std::string normalizeTimestampForPlotly(const std::string& rawTimestamp)
{
    std::string timestamp = trimCopy(rawTimestamp);

    // Some CSV writers serialize integer timestamps as 20200101.0.
    if (timestamp.size() > 2U &&
        timestamp.compare(timestamp.size() - 2U, 2U, ".0") == 0) {
        const std::string withoutDecimal =
            timestamp.substr(0U, timestamp.size() - 2U);
        if (isAllDigits(withoutDecimal)) {
            timestamp = withoutDecimal;
        }
    }

    if (looksLikeCalendarTimestamp(timestamp)) {
        std::string result =
            timestamp.substr(0U, 4U) + "-" +
            timestamp.substr(4U, 2U) + "-" +
            timestamp.substr(6U, 2U);

        if (timestamp.size() >= 10U) {
            result += "T" + timestamp.substr(8U, 2U);
            result += timestamp.size() >= 12U
                ? ":" + timestamp.substr(10U, 2U)
                : ":00";
            result += timestamp.size() >= 14U
                ? ":" + timestamp.substr(12U, 2U)
                : ":00";
        }

        return result;
    }

    if (isAllDigits(timestamp) &&
        (timestamp.size() == 10U || timestamp.size() == 13U)) {
        try {
            return formatUnixTimestampForPlotly(std::stoll(timestamp));
        } catch (...) {
            return timestamp;
        }
    }

    // Plotly understands ISO-style date strings more reliably than a date and
    // time separated by a blank space.
    const std::size_t firstSpace = timestamp.find(' ');
    if (firstSpace != std::string::npos && timestamp.find('T') == std::string::npos) {
        timestamp[firstSpace] = 'T';
    }

    return timestamp;
}

std::vector<BenchmarkPoint> loadBenchmarkPointsFromCsv(
    const std::filesystem::path& csvPath,
    const std::string& benchmarkSymbol
)
{
    std::ifstream input(csvPath);
    if (!input.is_open()) {
        throw std::runtime_error(
            "Could not open dataset to build benchmark curve: " +
            csvPath.string()
        );
    }

    std::string headerLine;
    if (!std::getline(input, headerLine)) {
        throw std::runtime_error(
            "Dataset has no CSV header: " + csvPath.string()
        );
    }

    const char delimiter = detectCsvDelimiter(headerLine);
    const std::vector<std::string> headers = parseCsvRow(headerLine, delimiter);

    const std::size_t timestampColumn = findCsvColumn(
        headers,
        {
            "timestamp", "datetime", "date", "time", "open_time",
            "opentime", "unix", "unix_timestamp"
        }
    );
    const std::size_t symbolColumn = findCsvColumn(
        headers,
        {"symbol", "ticker", "asset", "pair", "instrument"}
    );
    const std::size_t closeColumn = findCsvColumn(
        headers,
        {"close", "close_price", "closeprice", "price"}
    );

    if (timestampColumn == std::numeric_limits<std::size_t>::max() ||
        symbolColumn == std::numeric_limits<std::size_t>::max() ||
        closeColumn == std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error(
            "Could not locate timestamp/date, symbol and close columns in: " +
            csvPath.string()
        );
    }

    const std::size_t requiredColumn = std::max(
        timestampColumn,
        std::max(
            closeColumn,
            symbolColumn == std::numeric_limits<std::size_t>::max()
                ? 0U
                : symbolColumn
        )
    );

    std::vector<BenchmarkPoint> points;
    std::string line;

    while (std::getline(input, line)) {
        if (trimCopy(line).empty()) {
            continue;
        }

        const std::vector<std::string> fields = parseCsvRow(line, delimiter);
        if (fields.size() <= requiredColumn) {
            continue;
        }

        if (!benchmarkSymbolsMatch(
                fields[symbolColumn],
                benchmarkSymbol
            )) {
            continue;
        }

        double close = 0.0;
        if (!parseFiniteDouble(fields[closeColumn], close) || close <= 0.0) {
            continue;
        }

        const std::string timestamp =
            normalizeTimestampForPlotly(fields[timestampColumn]);
        if (timestamp.empty()) {
            continue;
        }

        points.push_back(BenchmarkPoint{timestamp, close});
    }

    if (points.empty()) {
        throw std::runtime_error(
            "No valid rows found for benchmark '" + benchmarkSymbol +
            "' in " + csvPath.string()
        );
    }

    // YYYY-MM-DD and ISO timestamps sort chronologically as strings. Sorting
    // also protects against datasets whose rows are grouped by symbol rather
    // than globally ordered by timestamp.
    std::sort(
        points.begin(),
        points.end(),
        [](const BenchmarkPoint& left, const BenchmarkPoint& right) {
            return left.timestamp < right.timestamp;
        }
    );

    // Keep the last close when the CSV contains the same symbol/timestamp more
    // than once.
    std::vector<BenchmarkPoint> uniquePoints;
    uniquePoints.reserve(points.size());

    for (const BenchmarkPoint& point : points) {
        if (!uniquePoints.empty() &&
            uniquePoints.back().timestamp == point.timestamp) {
            uniquePoints.back().close = point.close;
        } else {
            uniquePoints.push_back(point);
        }
    }

    return uniquePoints;
}

std::string escapeJavaScriptString(const std::string& value)
{
    std::ostringstream output;

    for (const unsigned char character : value) {
        switch (character) {
        case '\\':
            output << "\\\\";
            break;
        case '"':
            output << "\\\"";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        case '<':
            output << "\\u003c";
            break;
        case '>':
            output << "\\u003e";
            break;
        case '&':
            output << "\\u0026";
            break;
        default:
            if (character < 0x20U) {
                output << "\\u"
                       << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<unsigned int>(character)
                       << std::dec << std::setfill(' ');
            } else {
                output << static_cast<char>(character);
            }
            break;
        }
    }

    return output.str();
}

bool isJavaScriptQuote(const char character)
{
    return character == '\'' || character == '"' || character == '`';
}

std::size_t findTopLevelJavaScriptComma(
    const std::string& text,
    const std::size_t begin,
    const std::size_t end
)
{
    char quote = '\0';
    bool escaped = false;
    bool lineComment = false;
    bool blockComment = false;
    int parenthesesDepth = 0;
    int bracketsDepth = 0;
    int bracesDepth = 0;

    for (std::size_t index = begin; index < end; ++index) {
        const char character = text[index];
        const char next = index + 1U < end ? text[index + 1U] : '\0';

        if (lineComment) {
            if (character == '\n' || character == '\r') {
                lineComment = false;
            }
            continue;
        }

        if (blockComment) {
            if (character == '*' && next == '/') {
                blockComment = false;
                ++index;
            }
            continue;
        }

        if (quote != '\0') {
            if (escaped) {
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if (character == quote) {
                quote = '\0';
            }
            continue;
        }

        if (character == '/' && next == '/') {
            lineComment = true;
            ++index;
            continue;
        }

        if (character == '/' && next == '*') {
            blockComment = true;
            ++index;
            continue;
        }

        if (isJavaScriptQuote(character)) {
            quote = character;
            continue;
        }

        switch (character) {
        case '(':
            ++parenthesesDepth;
            break;
        case ')':
            --parenthesesDepth;
            break;
        case '[':
            ++bracketsDepth;
            break;
        case ']':
            --bracketsDepth;
            break;
        case '{':
            ++bracesDepth;
            break;
        case '}':
            --bracesDepth;
            break;
        case ',':
            if (parenthesesDepth == 0 && bracketsDepth == 0 &&
                bracesDepth == 0) {
                return index;
            }
            break;
        default:
            break;
        }
    }

    return std::string::npos;
}

std::string buildBuyAndHoldPlotlyTrace(
    const std::vector<BenchmarkPoint>& benchmarkPoints,
    const std::string& benchmarkSymbol,
    const double initialValue
)
{
    if (benchmarkPoints.empty()) {
        throw std::runtime_error(
            "Cannot build benchmark trace because it contains no points."
        );
    }

    const double firstClose = benchmarkPoints.front().close;
    if (!(firstClose > 0.0) || !std::isfinite(firstClose)) {
        throw std::runtime_error(
            "Cannot calculate buy-and-hold equity because the first close "
            "is invalid."
        );
    }

    // Equivalent formulas:
    //   quantity = initialValue / firstClose
    //   equity   = quantity * currentClose
    // Therefore equity = initialValue * currentClose / firstClose.
    std::ostringstream trace;
    trace << std::setprecision(17);

    trace << "{\"x\":[";
    for (std::size_t index = 0U; index < benchmarkPoints.size(); ++index) {
        if (index != 0U) {
            trace << ',';
        }
        trace << '\"'
              << escapeJavaScriptString(benchmarkPoints[index].timestamp)
              << '\"';
    }

    trace << "],\"y\":[";
    for (std::size_t index = 0U; index < benchmarkPoints.size(); ++index) {
        if (index != 0U) {
            trace << ',';
        }

        const double buyAndHoldEquity =
            initialValue * benchmarkPoints[index].close / firstClose;

        trace << buyAndHoldEquity;
    }

    trace << "],\"name\":\""
          << escapeJavaScriptString(benchmarkSymbol)
          << " buy & hold equity (starts at 100,000)\""
          << ",\"type\":\"scatter\""
          << ",\"mode\":\"lines\""
          << ",\"showlegend\":true"
          << ",\"connectgaps\":false"
          << ",\"line\":{\"color\":\"#16a34a\",\"width\":3,"
             "\"dash\":\"dot\"}"
          << ",\"hovertemplate\":\"%{x}<br>"
          << escapeJavaScriptString(benchmarkSymbol)
          << " buy & hold equity: %{y:,.2f}<extra></extra>\""
          << '}';

    return trace.str();
}

std::string injectBuyAndHoldTraceIntoPlotlyData(
    std::string chartHtml,
    const std::vector<BenchmarkPoint>& benchmarkPoints,
    const std::string& benchmarkSymbol,
    const double initialValue
)
{
    const std::string benchmarkTrace = buildBuyAndHoldPlotlyTrace(
        benchmarkPoints,
        benchmarkSymbol,
        initialValue
    );

    // Modify the second argument of the final Plotly.newPlot call. Wrapping it
    // with [].concat(...) works whether the original chart helper passes a
    // literal array or a JavaScript variable containing the Equity/Balance
    // traces.
    std::size_t searchBefore = chartHtml.size();

    while (searchBefore != 0U) {
        const std::size_t callPosition = chartHtml.rfind(
            "Plotly.newPlot",
            searchBefore - 1U
        );

        if (callPosition == std::string::npos) {
            break;
        }

        const std::size_t openingParenthesis = chartHtml.find(
            '(',
            callPosition + std::string("Plotly.newPlot").size()
        );

        if (openingParenthesis == std::string::npos) {
            searchBefore = callPosition;
            continue;
        }

        const std::size_t firstComma = findTopLevelJavaScriptComma(
            chartHtml,
            openingParenthesis + 1U,
            chartHtml.size()
        );

        if (firstComma == std::string::npos) {
            searchBefore = callPosition;
            continue;
        }

        const std::size_t secondComma = findTopLevelJavaScriptComma(
            chartHtml,
            firstComma + 1U,
            chartHtml.size()
        );

        if (secondComma == std::string::npos) {
            searchBefore = callPosition;
            continue;
        }

        std::size_t dataBegin = firstComma + 1U;
        while (dataBegin < secondComma &&
               std::isspace(
                   static_cast<unsigned char>(chartHtml[dataBegin])
               ) != 0) {
            ++dataBegin;
        }

        std::size_t dataEnd = secondComma;
        while (dataEnd > dataBegin &&
               std::isspace(
                   static_cast<unsigned char>(chartHtml[dataEnd - 1U])
               ) != 0) {
            --dataEnd;
        }

        if (dataBegin >= dataEnd) {
            searchBefore = callPosition;
            continue;
        }

        const std::string originalDataArgument =
            chartHtml.substr(dataBegin, dataEnd - dataBegin);

        const std::string replacement =
            "[].concat((" + originalDataArgument + "),[" +
            benchmarkTrace + "])";

        chartHtml.replace(
            dataBegin,
            dataEnd - dataBegin,
            replacement
        );

        return chartHtml;
    }

    throw std::runtime_error(
        "Could not find the Plotly.newPlot data argument in the generated "
        "balance/equity HTML. The buy-and-hold line was not added."
    );
}


std::size_t findMatchingJavaScriptDelimiter(
    const std::string& text,
    const std::size_t openingPosition,
    const char openingCharacter,
    const char closingCharacter
)
{
    if (openingPosition >= text.size() ||
        text[openingPosition] != openingCharacter) {
        return std::string::npos;
    }

    char quote = '\0';
    bool escaped = false;
    bool lineComment = false;
    bool blockComment = false;
    int depth = 0;

    for (std::size_t index = openingPosition; index < text.size(); ++index) {
        const char character = text[index];
        const char next =
            index + 1U < text.size() ? text[index + 1U] : '\0';

        if (lineComment) {
            if (character == '\n' || character == '\r') {
                lineComment = false;
            }
            continue;
        }

        if (blockComment) {
            if (character == '*' && next == '/') {
                blockComment = false;
                ++index;
            }
            continue;
        }

        if (quote != '\0') {
            if (escaped) {
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if (character == quote) {
                quote = '\0';
            }
            continue;
        }

        if (character == '/' && next == '/') {
            lineComment = true;
            ++index;
            continue;
        }

        if (character == '/' && next == '*') {
            blockComment = true;
            ++index;
            continue;
        }

        if (isJavaScriptQuote(character)) {
            quote = character;
            continue;
        }

        if (character == openingCharacter) {
            ++depth;
        } else if (character == closingCharacter) {
            --depth;

            if (depth == 0) {
                return index;
            }
        }
    }

    return std::string::npos;
}

std::size_t findMatchingJavaScriptClosingBrace(
    const std::string& text,
    const std::size_t openingBrace
)
{
    return findMatchingJavaScriptDelimiter(
        text,
        openingBrace,
        '{',
        '}'
    );
}

bool looksLikeEquityTraceObject(const std::string& objectExpression)
{
    const bool hasX =
        objectExpression.find("\"x\"") != std::string::npos ||
        objectExpression.find("'x'") != std::string::npos ||
        objectExpression.find("x:") != std::string::npos ||
        objectExpression.find("x :") != std::string::npos;

    const bool hasY =
        objectExpression.find("\"y\"") != std::string::npos ||
        objectExpression.find("'y'") != std::string::npos ||
        objectExpression.find("y:") != std::string::npos ||
        objectExpression.find("y :") != std::string::npos;

    return hasX && hasY;
}

std::string extractNamedPlotlyTraceExpression(
    const std::string& chartHtml,
    const std::string& traceName
)
{
    const std::vector<std::string> nameMarkers{
        "\"name\":\"" + traceName + "\"",
        "\"name\": \"" + traceName + "\"",
        "'name':'" + traceName + "'",
        "'name': '" + traceName + "'",
        "name:\"" + traceName + "\"",
        "name: \"" + traceName + "\"",
        "name:'" + traceName + "'",
        "name: '" + traceName + "'"
    };

    for (const std::string& marker : nameMarkers) {
        std::size_t markerPosition = chartHtml.find(marker);

        while (markerPosition != std::string::npos) {
            std::size_t openingBrace =
                chartHtml.rfind('{', markerPosition);

            while (openingBrace != std::string::npos) {
                const std::size_t closingBrace =
                    findMatchingJavaScriptClosingBrace(
                        chartHtml,
                        openingBrace
                    );

                if (closingBrace != std::string::npos &&
                    closingBrace >= markerPosition) {
                    const std::string objectExpression =
                        chartHtml.substr(
                            openingBrace,
                            closingBrace - openingBrace + 1U
                        );

                    if (looksLikeEquityTraceObject(objectExpression)) {
                        return objectExpression;
                    }
                }

                if (openingBrace == 0U) {
                    break;
                }

                openingBrace = chartHtml.rfind(
                    '{',
                    openingBrace - 1U
                );
            }

            markerPosition = chartHtml.find(
                marker,
                markerPosition + marker.size()
            );
        }
    }

    throw std::runtime_error(
        "Could not extract the Plotly trace object named '" +
        traceName +
        "' from the generated balance/equity chart."
    );
}

bool isJavaScriptIdentifierStart(const char character)
{
    const unsigned char unsignedCharacter =
        static_cast<unsigned char>(character);

    return std::isalpha(unsignedCharacter) != 0 ||
        character == '_' ||
        character == '$';
}

bool isJavaScriptIdentifierCharacter(const char character)
{
    const unsigned char unsignedCharacter =
        static_cast<unsigned char>(character);

    return std::isalnum(unsignedCharacter) != 0 ||
        character == '_' ||
        character == '$';
}

bool isJavaScriptIdentifierExpression(const std::string& expression)
{
    const std::string trimmed = trimCopy(expression);

    if (trimmed.empty() || !isJavaScriptIdentifierStart(trimmed.front())) {
        return false;
    }

    return std::all_of(
        trimmed.begin() + 1,
        trimmed.end(),
        [](const char character) {
            return isJavaScriptIdentifierCharacter(character);
        }
    );
}

std::string stripOuterJavaScriptParentheses(std::string expression)
{
    expression = trimCopy(expression);

    while (
        expression.size() >= 2U &&
        expression.front() == '('
    ) {
        const std::size_t closing =
            findMatchingJavaScriptDelimiter(
                expression,
                0U,
                '(',
                ')'
            );

        if (closing != expression.size() - 1U) {
            break;
        }

        expression = trimCopy(
            expression.substr(1U, expression.size() - 2U)
        );
    }

    return expression;
}

std::string extractTopLevelObjectPropertyExpression(
    const std::string& objectExpression,
    const std::string& requestedProperty
)
{
    if (objectExpression.empty() || objectExpression.front() != '{') {
        throw std::runtime_error(
            "Cannot read Plotly trace properties because the extracted "
            "expression is not an object literal."
        );
    }

    std::size_t index = 1U;

    while (index < objectExpression.size()) {
        while (
            index < objectExpression.size() &&
            (
                std::isspace(
                    static_cast<unsigned char>(objectExpression[index])
                ) != 0 ||
                objectExpression[index] == ','
            )
        ) {
            ++index;
        }

        if (
            index >= objectExpression.size() ||
            objectExpression[index] == '}'
        ) {
            break;
        }

        std::string propertyName;

        if (
            objectExpression[index] == '"' ||
            objectExpression[index] == '\''
        ) {
            const char quote = objectExpression[index++];
            bool escaped = false;

            while (index < objectExpression.size()) {
                const char character = objectExpression[index++];

                if (escaped) {
                    propertyName += character;
                    escaped = false;
                } else if (character == '\\') {
                    escaped = true;
                } else if (character == quote) {
                    break;
                } else {
                    propertyName += character;
                }
            }
        } else if (isJavaScriptIdentifierStart(objectExpression[index])) {
            const std::size_t nameBegin = index;
            ++index;

            while (
                index < objectExpression.size() &&
                isJavaScriptIdentifierCharacter(objectExpression[index])
            ) {
                ++index;
            }

            propertyName = objectExpression.substr(
                nameBegin,
                index - nameBegin
            );
        } else {
            ++index;
            continue;
        }

        while (
            index < objectExpression.size() &&
            std::isspace(
                static_cast<unsigned char>(objectExpression[index])
            ) != 0
        ) {
            ++index;
        }

        if (
            index >= objectExpression.size() ||
            objectExpression[index] != ':'
        ) {
            continue;
        }

        ++index;

        while (
            index < objectExpression.size() &&
            std::isspace(
                static_cast<unsigned char>(objectExpression[index])
            ) != 0
        ) {
            ++index;
        }

        const std::size_t expressionBegin = index;
        char quote = '\0';
        bool escaped = false;
        bool lineComment = false;
        bool blockComment = false;
        int parenthesesDepth = 0;
        int bracketsDepth = 0;
        int bracesDepth = 0;

        for (; index < objectExpression.size(); ++index) {
            const char character = objectExpression[index];
            const char next =
                index + 1U < objectExpression.size()
                    ? objectExpression[index + 1U]
                    : '\0';

            if (lineComment) {
                if (character == '\n' || character == '\r') {
                    lineComment = false;
                }
                continue;
            }

            if (blockComment) {
                if (character == '*' && next == '/') {
                    blockComment = false;
                    ++index;
                }
                continue;
            }

            if (quote != '\0') {
                if (escaped) {
                    escaped = false;
                } else if (character == '\\') {
                    escaped = true;
                } else if (character == quote) {
                    quote = '\0';
                }
                continue;
            }

            if (character == '/' && next == '/') {
                lineComment = true;
                ++index;
                continue;
            }

            if (character == '/' && next == '*') {
                blockComment = true;
                ++index;
                continue;
            }

            if (isJavaScriptQuote(character)) {
                quote = character;
                continue;
            }

            switch (character) {
            case '(':
                ++parenthesesDepth;
                break;
            case ')':
                --parenthesesDepth;
                break;
            case '[':
                ++bracketsDepth;
                break;
            case ']':
                --bracketsDepth;
                break;
            case '{':
                ++bracesDepth;
                break;
            case '}':
                if (
                    parenthesesDepth == 0 &&
                    bracketsDepth == 0 &&
                    bracesDepth == 0
                ) {
                    const std::string expression = trimCopy(
                        objectExpression.substr(
                            expressionBegin,
                            index - expressionBegin
                        )
                    );

                    if (propertyName == requestedProperty) {
                        return expression;
                    }

                    return {};
                }

                --bracesDepth;
                break;
            case ',':
                if (
                    parenthesesDepth == 0 &&
                    bracketsDepth == 0 &&
                    bracesDepth == 0
                ) {
                    const std::string expression = trimCopy(
                        objectExpression.substr(
                            expressionBegin,
                            index - expressionBegin
                        )
                    );

                    if (propertyName == requestedProperty) {
                        return expression;
                    }

                    ++index;
                    break;
                }
                break;
            default:
                break;
            }

            if (
                character == ',' &&
                parenthesesDepth == 0 &&
                bracketsDepth == 0 &&
                bracesDepth == 0
            ) {
                break;
            }
        }
    }

    throw std::runtime_error(
        "Could not find property '" + requestedProperty +
        "' in the extracted Plotly Equity trace."
    );
}

std::string extractJavaScriptVariableInitializer(
    const std::string& chartHtml,
    const std::string& identifier
)
{
    const std::vector<std::string> declarationPrefixes{
        "const ",
        "let ",
        "var "
    };

    for (const std::string& prefix : declarationPrefixes) {
        const std::string marker = prefix + identifier;
        std::size_t position = chartHtml.find(marker);

        while (position != std::string::npos) {
            const std::size_t identifierEnd =
                position + marker.size();

            if (
                identifierEnd < chartHtml.size() &&
                isJavaScriptIdentifierCharacter(chartHtml[identifierEnd])
            ) {
                position = chartHtml.find(
                    marker,
                    identifierEnd
                );
                continue;
            }

            std::size_t cursor = identifierEnd;

            while (
                cursor < chartHtml.size() &&
                std::isspace(
                    static_cast<unsigned char>(chartHtml[cursor])
                ) != 0
            ) {
                ++cursor;
            }

            if (
                cursor >= chartHtml.size() ||
                chartHtml[cursor] != '='
            ) {
                position = chartHtml.find(
                    marker,
                    identifierEnd
                );
                continue;
            }

            ++cursor;

            while (
                cursor < chartHtml.size() &&
                std::isspace(
                    static_cast<unsigned char>(chartHtml[cursor])
                ) != 0
            ) {
                ++cursor;
            }

            if (cursor >= chartHtml.size()) {
                break;
            }

            const std::size_t expressionBegin = cursor;

            if (chartHtml[cursor] == '[') {
                const std::size_t expressionEnd =
                    findMatchingJavaScriptDelimiter(
                        chartHtml,
                        cursor,
                        '[',
                        ']'
                    );

                if (expressionEnd != std::string::npos) {
                    return chartHtml.substr(
                        expressionBegin,
                        expressionEnd - expressionBegin + 1U
                    );
                }
            } else if (chartHtml[cursor] == '{') {
                const std::size_t expressionEnd =
                    findMatchingJavaScriptDelimiter(
                        chartHtml,
                        cursor,
                        '{',
                        '}'
                    );

                if (expressionEnd != std::string::npos) {
                    return chartHtml.substr(
                        expressionBegin,
                        expressionEnd - expressionBegin + 1U
                    );
                }
            } else {
                char quote = '\0';
                bool escaped = false;
                int parenthesesDepth = 0;
                int bracketsDepth = 0;
                int bracesDepth = 0;

                for (; cursor < chartHtml.size(); ++cursor) {
                    const char character = chartHtml[cursor];

                    if (quote != '\0') {
                        if (escaped) {
                            escaped = false;
                        } else if (character == '\\') {
                            escaped = true;
                        } else if (character == quote) {
                            quote = '\0';
                        }
                        continue;
                    }

                    if (isJavaScriptQuote(character)) {
                        quote = character;
                        continue;
                    }

                    switch (character) {
                    case '(':
                        ++parenthesesDepth;
                        break;
                    case ')':
                        --parenthesesDepth;
                        break;
                    case '[':
                        ++bracketsDepth;
                        break;
                    case ']':
                        --bracketsDepth;
                        break;
                    case '{':
                        ++bracesDepth;
                        break;
                    case '}':
                        --bracesDepth;
                        break;
                    case ';':
                        if (
                            parenthesesDepth == 0 &&
                            bracketsDepth == 0 &&
                            bracesDepth == 0
                        ) {
                            return trimCopy(
                                chartHtml.substr(
                                    expressionBegin,
                                    cursor - expressionBegin
                                )
                            );
                        }
                        break;
                    default:
                        break;
                    }
                }
            }

            position = chartHtml.find(
                marker,
                identifierEnd
            );
        }
    }

    throw std::runtime_error(
        "Could not locate the JavaScript declaration for identifier '" +
        identifier + "'."
    );
}

std::string resolveJavaScriptArrayExpression(
    const std::string& chartHtml,
    std::string expression,
    const std::string& description
)
{
    constexpr unsigned int maximumResolutionDepth = 16U;

    for (
        unsigned int depth = 0U;
        depth < maximumResolutionDepth;
        ++depth
    ) {
        expression = stripOuterJavaScriptParentheses(
            std::move(expression)
        );

        if (!expression.empty() && expression.front() == '[') {
            const std::size_t closing =
                findMatchingJavaScriptDelimiter(
                    expression,
                    0U,
                    '[',
                    ']'
                );

            if (closing == expression.size() - 1U) {
                return expression;
            }
        }

        if (!isJavaScriptIdentifierExpression(expression)) {
            throw std::runtime_error(
                "The " + description +
                " expression could not be resolved to an array literal. "
                "Expression: " + expression
            );
        }

        expression = extractJavaScriptVariableInitializer(
            chartHtml,
            trimCopy(expression)
        );
    }

    throw std::runtime_error(
        "The " + description +
        " expression exceeded the maximum identifier-resolution depth."
    );
}

PlotlyXYExpressions extractNamedPlotlyXYExpressions(
    const std::string& chartHtml,
    const std::string& traceName
)
{
    const std::string traceObject =
        extractNamedPlotlyTraceExpression(
            chartHtml,
            traceName
        );

    const std::string xExpression =
        extractTopLevelObjectPropertyExpression(
            traceObject,
            "x"
        );

    const std::string yExpression =
        extractTopLevelObjectPropertyExpression(
            traceObject,
            "y"
        );

    PlotlyXYExpressions result;

    result.x = resolveJavaScriptArrayExpression(
        chartHtml,
        xExpression,
        traceName + " x/dates"
    );

    result.y = resolveJavaScriptArrayExpression(
        chartHtml,
        yExpression,
        traceName + " y/equity"
    );

    return result;
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

} // namespace

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    Logger::Instance().Setup(
        true,   // debug enabled
        false,  // quiet
        "",     // file appender
        "",     // rolling appender
        true    // include header
    );

    const std::filesystem::path databasesDir =
        "/mnt/c/Users/Juan/Documents/Python/algoTrading/storage/databases";

    const std::filesystem::path backtestsDir =
        "/mnt/c/Users/Juan/Documents/Python/algoTrading/storage/backtests";

    const std::filesystem::path studyDir =
        backtestsDir / "multi_strategy";

    if (!ensureDirectoryExists(studyDir)) {
        return 1;
    }

    const std::vector<DatasetDefinition> datasets{
        // Keep the benchmark regime SMA close to 50 calendar days on every timeframe.
        {"1d_cmc", "1d_cmc.csv", "BTC", 50},
        {"stocks_daily", "stocks_dailyB.csv", "^GSPC", 50},
        {"4h_binance", "4h_binance.csv", "BTCUSDT", 300},
        {"8h_binance", "8h_binance.csv", "BTCUSDT", 150},
        {"12h_binance", "12h_binance.csv", "BTCUSDT", 100},
        {"1d_binance", "1d_binance.csv", "BTCUSDT", 50},
        {"1d_4h_shift_binance", "1d_4h_shift_binance.csv", "BTCUSDT", 50},
        {"1d_8h_shift_binance", "1d_8h_shift_binance.csv", "BTCUSDT", 50},
        {"1d_12h_shift_binance", "1d_12h_shift_binance.csv", "BTCUSDT", 50},
        {"1d_16h_shift_binance", "1d_16h_shift_binance.csv", "BTCUSDT", 50},
        {"1d_20h_shift_binance", "1d_20h_shift_binance.csv", "BTCUSDT", 50},
        {"2d_binance", "2d_binance.csv", "BTCUSDT", 25},
        {"3d_binance", "3d_binance.csv", "BTCUSDT", 17},
        {"5d_binance", "5d_binance.csv", "BTCUSDT", 10},
        {"7d_binance", "7d_binance.csv", "BTCUSDT", 7},
        {"14d_binance", "14d_binance.csv", "BTCUSDT", 4},
        {"30d_binance", "30d_binance.csv", "BTCUSDT", 2},
        {"binance_oos", "binance_oos.csv", "BTCUSDT", 50},
    };

    constexpr double initialBalance = 100000.0;

    // Fees used by the backtest engine and by strategies receiving maker/taker fees.
    constexpr double feeTaker = 0.0;
    constexpr double feeMaker = 0.0;

    // Fees used by BargainChaser and ATRBreakout.
    constexpr double commissionEntryFactor = 0.0;
    constexpr double commissionExitFactor = 0.0;

    constexpr unsigned int topLiquidityCount = 20;

    auto makeTopLiquidityUniverse =
        [topLiquidityCount]() -> std::unique_ptr<UniverseSelector>
    {
        return std::make_unique<TopNLiquidityUniverse>(
            IndicatorSpec{
                IndicatorKind::SMA,
                PriceField::Volume,
                25
            },
            topLiquidityCount,
            true
        );
    };

    std::vector<StrategyDefinition> strategyDefinitions;

    // ------------------------------------------------------------------
    // BargainChaser
    // ------------------------------------------------------------------
#if ENABLE_BARGAIN_CHASER
    strategyDefinitions.push_back(StrategyDefinition{
        "BargainChaser",
        "bargain_chaser",
        [=](
            const std::string& benchmarkSymbol,
            unsigned int benchmarkMovingAverageLength
        ) -> std::unique_ptr<Strategy> {
            (void)benchmarkSymbol;
            (void)benchmarkMovingAverageLength;

            constexpr unsigned int maxPositionsOpen = 10;
            constexpr double riskPerTrade = 0.1;
            constexpr unsigned int maxRankingPosition = 999999;
            constexpr unsigned int barsUntilExit = 1;
            constexpr double fallPercentage = 10.0;
            constexpr unsigned int movingAverageLength = 50;

            auto universeSelector = makeTopLiquidityUniverse();
            auto ranker = std::make_unique<IndicatorRanker>(
                IndicatorSpec{
                    IndicatorKind::ROC,
                    PriceField::Close,
                    1
                },
                false
            );

            return std::make_unique<StrategyBargainChaser>(
                maxPositionsOpen,
                riskPerTrade,
                std::move(universeSelector),
                std::move(ranker),
                commissionEntryFactor,
                commissionExitFactor,
                maxRankingPosition,
                barsUntilExit,
                fallPercentage,
                movingAverageLength
            );
        }
    });
#endif

    // ------------------------------------------------------------------
    // ATRBreakout
    // ------------------------------------------------------------------
#if ENABLE_ATR_BREAKOUT
    strategyDefinitions.push_back(StrategyDefinition{
        "ATRBreakout",
        "atr_breakout",
        [=](
            const std::string& benchmarkSymbol,
            unsigned int benchmarkMovingAverageLength
        ) -> std::unique_ptr<Strategy> {
            (void)benchmarkSymbol;
            (void)benchmarkMovingAverageLength;

            constexpr unsigned int heldBars = 3;
            constexpr double atrMultiple = 0.875;
            constexpr unsigned int atrLength = 10;
            constexpr unsigned int momentumScoreNum = 30;
            constexpr double quantityPercent = 10.0;
            constexpr unsigned int maxPositionsOpen = 10;
            constexpr unsigned int maxRankingPosition = 999999;

            auto universeSelector = makeTopLiquidityUniverse();
            auto ranker = std::make_unique<IndicatorRanker>(
                IndicatorSpec{
                    IndicatorKind::ROC,
                    PriceField::Close,
                    momentumScoreNum
                },
                true
            );

            return std::make_unique<StrategyATRBreakout>(
                maxPositionsOpen,
                quantityPercent / 100.0,
                std::move(universeSelector),
                std::move(ranker),
                commissionEntryFactor,
                commissionExitFactor,
                maxRankingPosition,
                heldBars,
                atrMultiple,
                atrLength
            );
        }
    });
#endif

    // ------------------------------------------------------------------
    // MRShort
    // ------------------------------------------------------------------
#if ENABLE_MR_SHORT
    strategyDefinitions.push_back(StrategyDefinition{
        "MRShort",
        "mr_short",
        [=](
            const std::string& benchmarkSymbol,
            unsigned int benchmarkMovingAverageLength
        ) -> std::unique_ptr<Strategy> {
            constexpr unsigned int rsiLength = 5;
            constexpr double rsiEntry = 65.0;
            constexpr double entryAtrMultiple = 0.30;
            constexpr unsigned int entryAtrLength = 10;
            constexpr unsigned int heldBars = 7;
            constexpr double quantityPercent = 10.0;
            constexpr unsigned int maxPositionsOpen = 10;
            constexpr unsigned int maxRankingPosition = 1000000;

            auto universeSelector = makeTopLiquidityUniverse();
            auto ranker = std::make_unique<IndicatorRanker>(
                IndicatorSpec{
                    IndicatorKind::ROC,
                    PriceField::Close,
                    30
                },
                true
            );

            return std::make_unique<StrategyMRShort>(
                maxPositionsOpen,
                quantityPercent / 100.0,
                std::move(universeSelector),
                std::move(ranker),
                feeTaker,
                feeTaker,
                maxRankingPosition,
                rsiLength,
                rsiEntry,
                benchmarkMovingAverageLength,
                entryAtrMultiple,
                entryAtrLength,
                heldBars,
                benchmarkSymbol
            );
        }
    });
#endif

    // ------------------------------------------------------------------
    // PureMom
    // ------------------------------------------------------------------
#if ENABLE_PURE_MOM
    strategyDefinitions.push_back(StrategyDefinition{
        "PureMom",
        "pure_mom",
        [=](
            const std::string& benchmarkSymbol,
            unsigned int benchmarkMovingAverageLength
        ) -> std::unique_ptr<Strategy> {
            constexpr unsigned int heldBars = 7;
            constexpr unsigned int rocLength = 7;
            constexpr double quantityPercent = 10.0;
            constexpr unsigned int maxPositionsOpen = 3;
            constexpr unsigned int maxRankingPosition =
                std::numeric_limits<unsigned int>::max();

            auto universeSelector = makeTopLiquidityUniverse();
            auto ranker = std::make_unique<IndicatorRanker>(
                IndicatorSpec{
                    IndicatorKind::ROC,
                    PriceField::Close,
                    rocLength
                },
                true
            );

            return std::make_unique<StrategyPureMom>(
                maxPositionsOpen,
                quantityPercent / 100.0,
                std::move(universeSelector),
                std::move(ranker),
                feeTaker,
                feeTaker,
                maxRankingPosition,
                heldBars,
                benchmarkMovingAverageLength,
                benchmarkSymbol
            );
        }
    });
#endif

    // ------------------------------------------------------------------
    // PureRSI
    // ------------------------------------------------------------------
#if ENABLE_PURE_RSI
    strategyDefinitions.push_back(StrategyDefinition{
        "PureRSI",
        "pure_rsi",
        [=](
            const std::string& benchmarkSymbol,
            unsigned int benchmarkMovingAverageLength
        ) -> std::unique_ptr<Strategy> {
            (void)benchmarkSymbol;
            (void)benchmarkMovingAverageLength;

            constexpr unsigned int rsiLength = 7;
            constexpr double rsiEntry = 80.0;
            constexpr double rsiExit = 70.0;
            constexpr double quantityPercent = 10.0;
            constexpr unsigned int maxPositionsOpen = 10;
            constexpr unsigned int maxRankingPosition = 1000000;

            auto universeSelector = makeTopLiquidityUniverse();
            auto ranker = std::make_unique<IndicatorRanker>(
                IndicatorSpec{
                    IndicatorKind::RSI,
                    PriceField::Close,
                    rsiLength
                },
                true
            );

            return std::make_unique<StrategyPureRSI>(
                maxPositionsOpen,
                quantityPercent / 100.0,
                std::move(universeSelector),
                std::move(ranker),
                feeTaker,
                feeTaker,
                maxRankingPosition,
                rsiLength,
                rsiEntry,
                rsiExit
            );
        }
    });
#endif

    // ------------------------------------------------------------------
    // MRRSILong
    // ------------------------------------------------------------------
#if ENABLE_MR_RSI_LONG
    strategyDefinitions.push_back(StrategyDefinition{
        "MRRSILong",
        "mr_rsi_long",
        [=](
            const std::string& benchmarkSymbol,
            unsigned int benchmarkMovingAverageLength
        ) -> std::unique_ptr<Strategy> {
            (void)benchmarkSymbol;
            (void)benchmarkMovingAverageLength;

            constexpr unsigned int rsiLength = 3;
            constexpr double rsiEntryLevel = 10.0;
            constexpr unsigned int momentumLength = 30;
            constexpr unsigned int heldBars = 1;
            constexpr double quantityPercent = 10.0;
            constexpr unsigned int maxPositionsOpen = 10;
            constexpr unsigned int maxRankingPosition =
                std::numeric_limits<unsigned int>::max();

            auto universeSelector = makeTopLiquidityUniverse();
            auto ranker = std::make_unique<IndicatorRanker>(
                IndicatorSpec{
                    IndicatorKind::ROC,
                    PriceField::Close,
                    momentumLength
                },
                true
            );

            return std::make_unique<StrategyMRRSILong>(
                maxPositionsOpen,
                quantityPercent / 100.0,
                std::move(universeSelector),
                std::move(ranker),
                feeMaker,
                feeTaker,
                maxRankingPosition,
                rsiLength,
                rsiEntryLevel,
                heldBars
            );
        }
    });
#endif

    // ------------------------------------------------------------------
    // XHBreakout
    // ------------------------------------------------------------------
#if ENABLE_XH_BREAKOUT
    strategyDefinitions.push_back(StrategyDefinition{
        "XHBreakout",
        "xh_breakout",
        [=](
            const std::string& benchmarkSymbol,
            unsigned int benchmarkMovingAverageLength
        ) -> std::unique_ptr<Strategy> {
            (void)benchmarkSymbol;
            (void)benchmarkMovingAverageLength;

            constexpr unsigned int xH = 30;
            constexpr unsigned int fastMovingAverageLength = 5;
            constexpr unsigned int momentumLength = 30;
            constexpr double quantityPercent = 10.0;
            constexpr unsigned int maxPositionsOpen = 10;
            constexpr unsigned int maxRankingPosition = 9999999;

            auto universeSelector = makeTopLiquidityUniverse();
            auto ranker = std::make_unique<IndicatorRanker>(
                IndicatorSpec{
                    IndicatorKind::ROC,
                    PriceField::Close,
                    momentumLength
                },
                true
            );

            return std::make_unique<StrategyXHBreakout>(
                maxPositionsOpen,
                quantityPercent / 100.0,
                std::move(universeSelector),
                std::move(ranker),
                feeMaker,
                feeTaker,
                maxRankingPosition,
                xH,
                fastMovingAverageLength
            );
        }
    });
#endif


    // ------------------------------------------------------------------
    // DonchianBreakout
    // ------------------------------------------------------------------
#if ENABLE_DONCHIAN_BREAKOUT
    strategyDefinitions.push_back(StrategyDefinition{
        "DonchianBreakout",
        "donchian_breakout",
        [=](
            const std::string& benchmarkSymbol,
            unsigned int benchmarkMovingAverageLength
        ) -> std::unique_ptr<Strategy> {
            constexpr unsigned int donchianLookback = 30;
            constexpr bool useMarketStateFilter = false;
            constexpr unsigned int momentumLength = 30;
            constexpr double quantityPercent = 10.0;
            constexpr unsigned int maxPositionsOpen = 10;
            constexpr unsigned int maxRankingPosition = 9999999;

            auto universeSelector = makeTopLiquidityUniverse();
            auto ranker = std::make_unique<IndicatorRanker>(
                IndicatorSpec{
                    IndicatorKind::ROC,
                    PriceField::Close,
                    momentumLength
                },
                true
            );

            return std::make_unique<StrategyDonchianBreakout>(
                maxPositionsOpen,
                quantityPercent / 100.0,
                std::move(universeSelector),
                std::move(ranker),
                feeMaker,
                feeTaker,
                maxRankingPosition,
                donchianLookback,
                useMarketStateFilter,
                benchmarkMovingAverageLength,
                benchmarkSymbol
            );
        }
    });
#endif

    if (strategyDefinitions.empty()) {
        LG_ERROR("Enable at least one strategy.");
        return 1;
    }

    LG_INFO(
        "{} enabled strategies x {} datasets = {} isolated backtests planned",
        strategyDefinitions.size(),
        datasets.size(),
        strategyDefinitions.size() * datasets.size()
    );

    std::size_t successfulRuns = 0U;
    std::size_t failedRuns = 0U;

    // Only the enabled strategies that successfully complete the 1d_cmc
    // dataset are included in the cross-strategy equity/correlation report.
    std::vector<CorrelationChartEntry> oneDayCmcStrategyCharts;
    oneDayCmcStrategyCharts.reserve(strategyDefinitions.size());

    std::vector<BenchmarkPoint> oneDayCmcBtcPoints;
    std::string oneDayCmcBtcSymbol;

    for (const StrategyDefinition& strategyDefinition : strategyDefinitions) {
        std::vector<ChartEntry> chartEntries;
        chartEntries.reserve(datasets.size());

        for (const DatasetDefinition& dataset : datasets) {
            const std::filesystem::path databasePath =
                databasesDir / dataset.fileName;

            if (!std::filesystem::exists(databasePath)) {
                LG_ERROR(
                    "Skipping {} / {} because the dataset does not exist: {}",
                    strategyDefinition.name,
                    dataset.id,
                    databasePath.string()
                );
                ++failedRuns;
                continue;
            }

            const std::filesystem::path temporaryChartPath =
                studyDir /
                (
                    ".temporary_" + strategyDefinition.outputSlug + "_" +
                    dataset.id + "_balance_equity.html"
                );

            LG_INFO("============================================================");
            LG_INFO(
                "Running strategy={} dataset={} benchmark={} benchmarkSmaBars={}",
                strategyDefinition.name,
                dataset.fileName,
                dataset.benchmarkSymbol,
                dataset.benchmarkMovingAverageLength
            );

            try {
                // Load a fresh copy for every strategy/dataset pair to prevent
                // any state or indicator cache from leaking between runs.
                LG_INFO("Database loading started: {}", databasePath.string());
                OHLCVData ohlcvData = loadDatabase(
                    databasePath.string(),
                    00000000
                );
                LG_INFO("Database loaded successfully");

                unsigned int lastTradeId = 0U;
                double balance = initialBalance;
                double equity = initialBalance;

                std::vector<std::unique_ptr<Strategy>> strategies;
                strategies.push_back(
                    strategyDefinition.create(
                        dataset.benchmarkSymbol,
                        dataset.benchmarkMovingAverageLength
                    )
                );

                // This context contains exactly one strategy, so this is never
                // a portfolio backtest.
                BacktestContext context(
                    ohlcvData,
                    strategies,
                    balance,
                    equity,
                    lastTradeId,
                    feeMaker,
                    feeTaker,
                    false
                );

                const MarketData& marketData = context.GetMarketData();
                LG_INFO("Market data and indicators initialized");

                Backtester tester(context);

                LG_INFO("Starting loop");
                tester.loop();
                LG_INFO("Finished loop");

                tester.closeTrades();

                printBalanceEquityChart(
                    context.GetBalanceEquityHistoric(),
                    marketData,
                    temporaryChartPath.string()
                );

                std::string chartHtml = readTextFile(temporaryChartPath);

                const std::vector<BenchmarkPoint> benchmarkPoints =
                    loadBenchmarkPointsFromCsv(
                        databasePath,
                        dataset.benchmarkSymbol
                    );

                const double benchmarkFirstClose = benchmarkPoints.front().close;
                const double benchmarkLastEquity =
                    initialBalance * benchmarkPoints.back().close /
                    benchmarkFirstClose;

                LG_INFO(
                    "Buy-and-hold curve: symbol={} points={} first={} "
                    "firstClose={} last={} lastEquity={}",
                    dataset.benchmarkSymbol,
                    benchmarkPoints.size(),
                    benchmarkPoints.front().timestamp,
                    benchmarkFirstClose,
                    benchmarkPoints.back().timestamp,
                    benchmarkLastEquity
                );

                PlotlyXYExpressions oneDayCmcEquityCurve;

                if (dataset.id == "1d_cmc") {
                    oneDayCmcEquityCurve =
                        extractNamedPlotlyXYExpressions(
                            chartHtml,
                            "Equity"
                        );

                    LG_INFO(
                        "Saved resolved 1d_cmc Equity arrays for {} "
                        "(dates={} bytes, equity={} bytes)",
                        strategyDefinition.name,
                        oneDayCmcEquityCurve.x.size(),
                        oneDayCmcEquityCurve.y.size()
                    );
                }

                chartHtml = injectBuyAndHoldTraceIntoPlotlyData(
                    std::move(chartHtml),
                    benchmarkPoints,
                    dataset.benchmarkSymbol,
                    initialBalance
                );

                std::error_code removeError;
                std::filesystem::remove(temporaryChartPath, removeError);
                if (removeError) {
                    LG_WARN(
                        "Could not remove temporary chart '{}': {}",
                        temporaryChartPath.string(),
                        removeError.message()
                    );
                }

                if (dataset.id == "1d_cmc") {
                    oneDayCmcStrategyCharts.push_back(
                        CorrelationChartEntry{
                            strategyDefinition.name,
                            std::move(oneDayCmcEquityCurve.x),
                            std::move(oneDayCmcEquityCurve.y)
                        }
                    );

                    if (oneDayCmcBtcPoints.empty()) {
                        oneDayCmcBtcPoints = benchmarkPoints;
                        oneDayCmcBtcSymbol = dataset.benchmarkSymbol;
                    }
                }

                chartEntries.push_back(ChartEntry{
                    dataset.id,
                    dataset.fileName,
                    dataset.benchmarkSymbol,
                    std::move(chartHtml)
                });

                ++successfulRuns;

                LG_INFO(
                    "Completed strategy={} dataset={}",
                    strategyDefinition.name,
                    dataset.id
                );
            } catch (const std::exception& exception) {
                ++failedRuns;
                LG_ERROR(
                    "Backtest failed for strategy={} dataset={}: {}",
                    strategyDefinition.name,
                    dataset.id,
                    exception.what()
                );
            } catch (...) {
                ++failedRuns;
                LG_ERROR(
                    "Backtest failed for strategy={} dataset={}: "
                    "unknown non-standard exception",
                    strategyDefinition.name,
                    dataset.id
                );
            }
        }

        if (!chartEntries.empty()) {
            try {
                const std::filesystem::path indexPath =
                    studyDir /
                    (strategyDefinition.outputSlug + "_all_datasets.html");

                writeStrategyIndexHtml(
                    indexPath,
                    strategyDefinition.name,
                    chartEntries
                );

                LG_INFO(
                    "Combined strategy HTML written: {}",
                    indexPath.string()
                );
            } catch (const std::exception& exception) {
                ++failedRuns;
                LG_ERROR(
                    "Could not write combined HTML for {}: {}",
                    strategyDefinition.name,
                    exception.what()
                );
            }
        }
    }

    if (!oneDayCmcStrategyCharts.empty() &&
        !oneDayCmcBtcPoints.empty()) {
        try {
            const std::filesystem::path correlationPath =
                studyDir / "1d_cmc_strategy_equity_correlations.html";

            write1dCmcCorrelationHtml(
                correlationPath,
                oneDayCmcStrategyCharts,
                oneDayCmcBtcPoints,
                oneDayCmcBtcSymbol,
                initialBalance
            );

            LG_INFO(
                "1d_cmc equity/correlation HTML written: {}",
                correlationPath.string()
            );
        } catch (const std::exception& exception) {
            ++failedRuns;
            LG_ERROR(
                "Could not write the 1d_cmc equity/correlation HTML: {}",
                exception.what()
            );
        }
    } else {
        LG_WARN(
            "The 1d_cmc equity/correlation HTML was not created because "
            "no successful 1d_cmc strategy curves or BTC benchmark points "
            "were collected."
        );
    }

    LG_INFO("============================================================");
    LG_INFO(
        "Multi-dataset study completed: {} successful runs, {} failed/skipped runs",
        successfulRuns,
        failedRuns
    );
    LG_INFO("Outputs: {}", studyDir.string());

    return successfulRuns > 0U ? 0 : 1;
}
