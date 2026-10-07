// Parses historical CSV data one day at a time and enforces ordering, validation, and anti-
// lookahead boundaries.

#include "historical_csv_source.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {

Timestamp parseDate(const std::string& text)
{
    if (text.size() != 10 || text[4] != '-' || text[7] != '-')
        throw std::runtime_error("Historical CSV date must use YYYY-MM-DD: " + text);

    std::string compact;
    compact.reserve(8);
    for (const char c : text) {
        if (c != '-')
            compact.push_back(c);
    }

    const unsigned long value = std::stoul(compact);
    if (value == 0 || value > static_cast<unsigned long>(std::numeric_limits<Timestamp>::max()))
        throw std::runtime_error("Historical CSV date is out of Timestamp range: " + text);
    return static_cast<Timestamp>(value);
}

bool validBar(const OHLCV& bar)
{
    if (!std::isfinite(bar.open) || bar.open <= 0.0 ||
        !std::isfinite(bar.high) || bar.high <= 0.0 ||
        !std::isfinite(bar.low) || bar.low <= 0.0 ||
        !std::isfinite(bar.close) || bar.close <= 0.0 ||
        !std::isfinite(bar.volume) || bar.volume < 0.0)
        return false;

    return bar.high >= std::max({bar.open, bar.close, bar.low}) &&
           bar.low <= std::min({bar.open, bar.close, bar.high});
}

std::vector<std::string> splitCsv(const std::string& line)
{
    std::vector<std::string> fields;
    std::stringstream stream(line);
    std::string field;
    while (std::getline(stream, field, ','))
        fields.push_back(field);
    if (!line.empty() && line.back() == ',')
        fields.emplace_back();
    return fields;
}

} // namespace

std::size_t HistoricalMarketDay::rowCount() const noexcept
{
    std::size_t result = 0;
    for (const auto& [_, byDate] : bars.data)
        result += byDate.size();
    return result;
}

HistoricalCsvSource::HistoricalCsvSource(std::filesystem::path path)
    : path_(std::move(path)), input_(path_)
{
    if (path_.empty())
        throw std::invalid_argument("Historical CSV path cannot be empty");
    if (!input_)
        throw std::runtime_error("Cannot open historical CSV: " + path_.string());

    std::string header;
    if (!std::getline(input_, header))
        throw std::runtime_error("Historical CSV has no header: " + path_.string());

    if (!header.empty() && static_cast<unsigned char>(header[0]) == 0xEF) {
        // Strip UTF-8 BOM when present.
        if (header.size() >= 3 &&
            static_cast<unsigned char>(header[1]) == 0xBB &&
            static_cast<unsigned char>(header[2]) == 0xBF)
            header.erase(0, 3);
    }

    if (header != "date,symbol,open,high,low,close,volume")
        throw std::runtime_error(
            "Historical CSV header must be date,symbol,open,high,low,close,volume");
}

HistoricalCsvSource::ParsedRow HistoricalCsvSource::parseLine(
    const std::string& line,
    std::size_t lineNumber) const
{
    const auto fields = splitCsv(line);
    if (fields.size() != 7)
        throw std::runtime_error(
            "Historical CSV line " + std::to_string(lineNumber) + " must contain 7 columns");
    if (fields[0].empty() || fields[1].empty())
        throw std::runtime_error(
            "Historical CSV line " + std::to_string(lineNumber) + " has empty date/symbol");

    ParsedRow result;
    result.date = parseDate(fields[0]);
    result.symbol = fields[1];

    try {
        result.bar.open = std::stod(fields[2]);
        result.bar.high = std::stod(fields[3]);
        result.bar.low = std::stod(fields[4]);
        result.bar.close = std::stod(fields[5]);
        result.bar.volume = std::stod(fields[6]);
    }
    catch (const std::exception&) {
        throw std::runtime_error(
            "Historical CSV line " + std::to_string(lineNumber) + " has invalid numeric data");
    }

    if (!validBar(result.bar))
        throw std::runtime_error(
            "Historical CSV line " + std::to_string(lineNumber) + " has invalid OHLCV");

    return result;
}

std::optional<Timestamp> HistoricalCsvSource::peekDateOnly()
{
    while (true) {
        const std::streampos position = input_.tellg();
        if (position == std::streampos(-1)) {
            input_.clear();
            return std::nullopt;
        }

        std::string dateField;
        char ch = '\0';
        bool sawAny = false;
        bool sawComma = false;

        while (input_.get(ch)) {
            if (ch == '\n')
                break;
            sawAny = true;
            if (ch == ',') {
                sawComma = true;
                break;
            }
            if (ch != '\r')
                dateField.push_back(ch);
        }

        input_.clear();
        input_.seekg(position);
        if (!input_)
            throw std::runtime_error("Cannot rewind historical CSV while peeking date");

        if (!sawAny) {
            // Consume blank lines so the cursor remains monotonic.
            std::string blank;
            if (!std::getline(input_, blank)) {
                input_.clear();
                return std::nullopt;
            }
            ++line_number_;
            continue;
        }

        if (!sawComma)
            throw std::runtime_error(
                "Historical CSV line " + std::to_string(line_number_ + 1) +
                " has no comma after date");

        return parseDate(dateField);
    }
}

std::optional<Timestamp> HistoricalCsvSource::nextDate()
{
    const auto date = peekDateOnly();
    if (!date.has_value())
        return std::nullopt;
    if (last_consumed_date_ != 0 && *date < last_consumed_date_)
        throw std::runtime_error("Historical CSV dates move backwards");
    return date;
}

HistoricalMarketDay HistoricalCsvSource::readNextDay()
{
    const auto firstDate = nextDate();
    if (!firstDate.has_value())
        throw std::runtime_error("Historical CSV is exhausted");

    if (last_consumed_date_ != 0 && *firstDate <= last_consumed_date_)
        throw std::runtime_error("Historical CSV date was already consumed or moves backwards");

    HistoricalMarketDay result;
    result.date = *firstDate;
    std::set<std::string> symbols;

    while (true) {
        const auto currentDate = nextDate();
        if (!currentDate.has_value())
            break;
        if (*currentDate < result.date)
            throw std::runtime_error("Historical CSV dates move backwards");
        if (*currentDate > result.date)
            break; // Crucially, no future OHLCV row has been consumed or parsed.

        std::string line;
        if (!std::getline(input_, line))
            throw std::runtime_error("Historical CSV ended while reading visible day");
        ++line_number_;
        if (line.empty())
            continue;

        const ParsedRow row = parseLine(line, line_number_);
        if (row.date != result.date)
            throw std::logic_error("Historical CSV date changed between peek and consume");

        if (!symbols.insert(row.symbol).second)
            throw std::runtime_error(
                "Historical CSV contains duplicate symbol/date: " + row.symbol + "/" +
                std::to_string(row.date));

        result.bars.data[row.symbol][row.date] = row.bar;
        // The reference CSV exposes `volume`, not a venue-specific quote-volume field.
        // We rank deterministically by the dataset's own volume value. The replay
        // market_top_n is deliberately configured high enough to keep every symbol.
        result.volume_ranking.emplace_back(row.symbol, row.bar.volume);
    }

    if (result.bars.data.empty())
        throw std::runtime_error("Historical CSV day contains no rows");

    std::sort(
        result.volume_ranking.begin(),
        result.volume_ranking.end(),
        [](const auto& left, const auto& right) {
            if (left.second != right.second)
                return left.second > right.second;
            return left.first < right.first;
        });

    last_consumed_date_ = result.date;
    return result;
}

std::optional<HistoricalMarketDay> HistoricalCsvSource::skipThrough(Timestamp checkpointDate)
{
    if (checkpointDate == 0)
        return std::nullopt;

    std::optional<HistoricalMarketDay> checkpointDay;
    while (const auto next = nextDate()) {
        if (*next > checkpointDate)
            break;

        HistoricalMarketDay consumed = readNextDay();
        if (consumed.date == checkpointDate) {
            checkpointDay = std::move(consumed);
            break;
        }
    }

    if (!checkpointDay.has_value())
        throw std::runtime_error(
            "Canonical market-data checkpoint is not present in historical CSV source: " +
            std::to_string(checkpointDate));

    return checkpointDay;
}


HistoricalOpenCsvSource::HistoricalOpenCsvSource(std::filesystem::path path)
    : path_(std::move(path)), input_(path_)
{
    if (path_.empty())
        throw std::invalid_argument("Historical open CSV path cannot be empty");
    if (!input_)
        throw std::runtime_error("Cannot open historical open CSV: " + path_.string());

    std::string header;
    if (!std::getline(input_, header))
        throw std::runtime_error("Historical open CSV has no header: " + path_.string());

    if (!header.empty() && static_cast<unsigned char>(header[0]) == 0xEF) {
        if (header.size() >= 3 &&
            static_cast<unsigned char>(header[1]) == 0xBB &&
            static_cast<unsigned char>(header[2]) == 0xBF)
            header.erase(0, 3);
    }

    if (header != "date,symbol,open,high,low,close,volume")
        throw std::runtime_error(
            "Historical open CSV header must be date,symbol,open,high,low,close,volume");
}


HistoricalOpenCsvSource::ParsedOpenRow HistoricalOpenCsvSource::parseOpenLine(
    const std::string& line,
    std::size_t lineNumber) const
{
    // Parse exactly the visible-at-open prefix.  Do not parse high/low/close/volume.
    std::stringstream stream(line);
    std::string dateField;
    std::string symbol;
    std::string openField;

    if (!std::getline(stream, dateField, ',') ||
        !std::getline(stream, symbol, ',') ||
        !std::getline(stream, openField, ','))
        throw std::runtime_error(
            "Historical open CSV line " + std::to_string(lineNumber) +
            " must contain date,symbol,open prefix");

    if (dateField.empty() || symbol.empty() || openField.empty())
        throw std::runtime_error(
            "Historical open CSV line " + std::to_string(lineNumber) +
            " has empty date/symbol/open");

    ParsedOpenRow result;
    result.date = parseDate(dateField);
    result.symbol = std::move(symbol);
    try {
        result.open = std::stod(openField);
    }
    catch (const std::exception&) {
        throw std::runtime_error(
            "Historical open CSV line " + std::to_string(lineNumber) +
            " has invalid open price");
    }

    if (!std::isfinite(result.open) || result.open <= 0.0)
        throw std::runtime_error(
            "Historical open CSV line " + std::to_string(lineNumber) +
            " has invalid open price");

    return result;
}


std::optional<Timestamp> HistoricalOpenCsvSource::peekDateOnly()
{
    while (true) {
        const std::streampos position = input_.tellg();
        if (position == std::streampos(-1)) {
            input_.clear();
            return std::nullopt;
        }

        std::string dateField;
        char ch = '\0';
        bool sawAny = false;
        bool sawComma = false;

        while (input_.get(ch)) {
            if (ch == '\n')
                break;
            sawAny = true;
            if (ch == ',') {
                sawComma = true;
                break;
            }
            if (ch != '\r')
                dateField.push_back(ch);
        }

        input_.clear();
        input_.seekg(position);
        if (!input_)
            throw std::runtime_error("Cannot rewind historical open CSV while peeking date");

        if (!sawAny) {
            std::string blank;
            if (!std::getline(input_, blank)) {
                input_.clear();
                return std::nullopt;
            }
            ++line_number_;
            continue;
        }

        if (!sawComma)
            throw std::runtime_error(
                "Historical open CSV line " + std::to_string(line_number_ + 1) +
                " has no comma after date");

        return parseDate(dateField);
    }
}


std::optional<Timestamp> HistoricalOpenCsvSource::nextDate()
{
    const auto date = peekDateOnly();
    if (!date.has_value())
        return std::nullopt;
    if (last_consumed_date_ != 0 && *date < last_consumed_date_)
        throw std::runtime_error("Historical open CSV dates move backwards");
    return date;
}


HistoricalExecutionOpen HistoricalOpenCsvSource::readNextDay()
{
    const auto firstDate = nextDate();
    if (!firstDate.has_value())
        throw std::runtime_error("Historical open CSV is exhausted");

    if (last_consumed_date_ != 0 && *firstDate <= last_consumed_date_)
        throw std::runtime_error(
            "Historical open CSV date was already consumed or moves backwards");

    HistoricalExecutionOpen result;
    result.date = *firstDate;
    std::set<std::string> symbols;

    while (true) {
        const auto currentDate = nextDate();
        if (!currentDate.has_value())
            break;
        if (*currentDate < result.date)
            throw std::runtime_error("Historical open CSV dates move backwards");
        if (*currentDate > result.date)
            break;

        std::string line;
        if (!std::getline(input_, line))
            throw std::runtime_error("Historical open CSV ended while reading visible open");
        ++line_number_;
        if (line.empty())
            continue;

        ParsedOpenRow row = parseOpenLine(line, line_number_);
        if (row.date != result.date)
            throw std::logic_error("Historical open CSV date changed between peek and consume");
        if (!symbols.insert(row.symbol).second)
            throw std::runtime_error(
                "Historical open CSV contains duplicate symbol/date: " + row.symbol + "/" +
                std::to_string(row.date));
        result.prices.emplace(std::move(row.symbol), row.open);
    }

    if (result.prices.empty())
        throw std::runtime_error("Historical open CSV day contains no rows");

    last_consumed_date_ = result.date;
    return result;
}
