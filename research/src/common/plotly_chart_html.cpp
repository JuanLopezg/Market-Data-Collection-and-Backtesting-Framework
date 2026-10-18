#include "plotly_chart_html.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

// Generated Plotly HTML is scanned as JavaScript text: string literals, nested
// brackets and top-level commas are tracked so trace objects and the arrays they
// reference can be found without a JavaScript engine.
namespace scenario_reports {

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

std::vector<BenchmarkPoint> makeBenchmarkPoints(
    const MarketData& market, const std::string& symbol)
{
    std::vector<BenchmarkPoint> points;
    for (const auto& [timestamp, bars] : market) {
        const auto bar = bars.find(symbol);
        if (bar != bars.end() && std::isfinite(bar->second.close) && bar->second.close > 0.0)
            points.push_back({normalizeTimestampForPlotly(std::to_string(timestamp)), bar->second.close});
    }
    if (points.empty())
        throw std::runtime_error("Selected input has no valid benchmark bars: " + symbol);
    return points;
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

} // namespace scenario_reports
