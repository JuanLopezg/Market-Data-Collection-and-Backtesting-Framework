#include "realtest.h"
#include "logger.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
constexpr double BASE_DYNAMIC_TOLERANCE_PERCENT = 20.0;
constexpr double EXTRA_DYNAMIC_TOLERANCE_PER_TRADE_ID = 0.2;
constexpr double MAX_DYNAMIC_TOLERANCE_PERCENT = 50.0;
}

struct RealtestTrade {
    std::string trade;
    std::string strategy;
    std::string symbol;
    std::string side;

    std::string dateIn;
    std::string timeIn;
    double qtyIn = 0.0;
    double priceIn = 0.0;

    std::string dateOut;
    std::string timeOut;
    double qtyOut = 0.0;
    double priceOut = 0.0;

    std::string reason;
    int bars = 0;

    double pctGain = 0.0;
    double profit = 0.0;
    double pctMFE = 0.0;
    double pctMAE = 0.0;
    double fraction = 0.0;
    double size = 0.0;
    double dividends = 0.0;
};

static std::string trim(std::string s)
{
    auto notSpace = [](unsigned char c) {
        return !std::isspace(c);
    };

    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());

    return s;
}

static std::string toLower(std::string value)
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

static std::vector<std::string> parseCsvLine(const std::string& line)
{
    std::vector<std::string> fields;
    std::string current;
    bool inQuotes = false;

    for (std::size_t i = 0; i < line.size(); ++i) {
        char c = line[i];

        if (c == '"') {
            if (inQuotes && i + 1 < line.size() && line[i + 1] == '"') {
                current += '"';
                ++i;
            } else {
                inQuotes = !inQuotes;
            }
        } else if (c == ',' && !inQuotes) {
            fields.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }

    fields.push_back(current);
    return fields;
}

static double parseRealtestNumber(std::string value)
{
    value = trim(value);

    if (value.empty()) {
        return 0.0;
    }

    bool negativeByParentheses = false;

    if (value.size() >= 2 && value.front() == '(' && value.back() == ')') {
        negativeByParentheses = true;
        value = value.substr(1, value.size() - 2);
        value = trim(value);
    }

    value.erase(
        std::remove_if(
            value.begin(),
            value.end(),
            [](unsigned char c) {
                return c == '$' || c == '%' || c == ',' || std::isspace(c);
            }
        ),
        value.end()
    );

    if (value.empty()) {
        return 0.0;
    }

    const double result = std::stod(value);

    return negativeByParentheses ? -result : result;
}

static int parseRealtestInt(std::string value)
{
    value = trim(value);

    if (value.empty()) {
        return 0;
    }

    return std::stoi(value);
}

static bool loadRealtestTrades(
    const std::filesystem::path& realtestTradesPath,
    std::vector<RealtestTrade>& realtestTrades
) {
    constexpr const char* EXPECTED_HEADER =
        "Trade,Strategy,Symbol,Side,DateIn,TimeIn,QtyIn,PriceIn,"
        "DateOut,TimeOut,QtyOut,PriceOut,Reason,Bars,PctGain,Profit,"
        "PctMFE,PctMAE,Fraction,Size,Dividends";

    realtestTrades.clear();

    std::ifstream file(realtestTradesPath);

    if (!file.is_open()) {
        LG_ERROR("Could not open RealTest trades CSV: {}", realtestTradesPath.string());
        return false;
    }

    std::string header;

    if (!std::getline(file, header)) {
        LG_ERROR("RealTest trades CSV is empty: {}", realtestTradesPath.string());
        return false;
    }

    if (!header.empty() && header.back() == '\r') {
        header.pop_back();
    }

    if (header != EXPECTED_HEADER) {
        LG_ERROR("Invalid RealTest trades CSV header in file: {}", realtestTradesPath.string());
        LG_ERROR("Expected header: {}", EXPECTED_HEADER);
        LG_ERROR("Received header: {}", header);
        return false;
    }

    LG_INFO("RealTest trades CSV header validated: {}", realtestTradesPath.string());

    std::string line;
    std::size_t lineNumber = 1;

    while (std::getline(file, line)) {
        ++lineNumber;

        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (trim(line).empty()) {
            LG_INFO("Skipping empty line {} in RealTest trades CSV", lineNumber);
            continue;
        }

        std::vector<std::string> fields = parseCsvLine(line);

        if (fields.size() != 21) {
            LG_ERROR(
                "Invalid field count on line {} in RealTest trades CSV. Expected 21 fields, got {}",
                lineNumber,
                fields.size()
            );
            return false;
        }

        try {
            RealtestTrade realtestTrade;

            realtestTrade.trade     = trim(fields[0]);
            realtestTrade.strategy  = trim(fields[1]);
            realtestTrade.symbol    = trim(fields[2]);
            realtestTrade.side      = trim(fields[3]);

            realtestTrade.dateIn    = trim(fields[4]);
            realtestTrade.timeIn    = trim(fields[5]);
            realtestTrade.qtyIn     = parseRealtestNumber(fields[6]);
            realtestTrade.priceIn   = parseRealtestNumber(fields[7]);

            realtestTrade.dateOut   = trim(fields[8]);
            realtestTrade.timeOut   = trim(fields[9]);
            realtestTrade.qtyOut    = parseRealtestNumber(fields[10]);
            realtestTrade.priceOut  = parseRealtestNumber(fields[11]);

            realtestTrade.reason    = trim(fields[12]);
            realtestTrade.bars      = parseRealtestInt(fields[13]);

            realtestTrade.pctGain   = parseRealtestNumber(fields[14]);
            realtestTrade.profit    = parseRealtestNumber(fields[15]);
            realtestTrade.pctMFE    = parseRealtestNumber(fields[16]);
            realtestTrade.pctMAE    = parseRealtestNumber(fields[17]);
            realtestTrade.fraction  = parseRealtestNumber(fields[18]);
            realtestTrade.size      = parseRealtestNumber(fields[19]);
            realtestTrade.dividends = parseRealtestNumber(fields[20]);

            realtestTrades.push_back(std::move(realtestTrade));
        } catch (const std::exception& e) {
            LG_ERROR(
                "Failed to parse RealTest trades CSV on line {}: {}",
                lineNumber,
                e.what()
            );
            return false;
        }
    }

    LG_INFO(
        "Loaded {} RealTest trades from CSV: {}",
        realtestTrades.size(),
        realtestTradesPath.string()
    );

    return true;
}

static std::int64_t daysFromCivil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;

    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy =
        (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe =
        yoe * 365 + yoe / 4 - yoe / 100 + doy;

    return era * 146097 + static_cast<int>(doe) - 719468;
}

static void civilFromDays(
    std::int64_t z,
    int& year,
    unsigned& month,
    unsigned& day
) {
    z += 719468;

    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;

    day = doy - (153 * mp + 2) / 5 + 1;
    month = mp + (mp < 10 ? 3 : -9);
    year = static_cast<int>(y + (month <= 2));
}

static bool isPackedYYYYMMDD(std::int64_t value)
{
    if (value < 19000101 || value > 29991231) {
        return false;
    }

    const int year = static_cast<int>(value / 10000);
    const int month = static_cast<int>((value / 100) % 100);
    const int day = static_cast<int>(value % 100);

    return year >= 1900 &&
           year <= 2999 &&
           month >= 1 &&
           month <= 12 &&
           day >= 1 &&
           day <= 31;
}

static Timestamp normalizeTimestamp(Timestamp timestamp)
{
    const std::int64_t raw = static_cast<std::int64_t>(timestamp);

    if (!isPackedYYYYMMDD(raw)) {
        return timestamp;
    }

    const int year = static_cast<int>(raw / 10000);
    const unsigned month = static_cast<unsigned>((raw / 100) % 100);
    const unsigned day = static_cast<unsigned>(raw % 100);

    const std::int64_t days = daysFromCivil(year, month, day);
    const std::int64_t unixSeconds = days * 86400LL;

    return static_cast<Timestamp>(unixSeconds);
}

static std::string timestampToString(Timestamp timestamp)
{
    timestamp = normalizeTimestamp(timestamp);

    std::int64_t unixSeconds = static_cast<std::int64_t>(timestamp);

    std::int64_t days = unixSeconds / 86400LL;
    std::int64_t secondsOfDay = unixSeconds % 86400LL;

    if (secondsOfDay < 0) {
        secondsOfDay += 86400LL;
        --days;
    }

    int year = 0;
    unsigned month = 0;
    unsigned day = 0;

    civilFromDays(days, year, month, day);

    const int hour = static_cast<int>(secondsOfDay / 3600LL);
    secondsOfDay %= 3600LL;

    const int minute = static_cast<int>(secondsOfDay / 60LL);
    const int second = static_cast<int>(secondsOfDay % 60LL);

    std::ostringstream output;

    output
        << std::setfill('0')
        << std::setw(4) << year << "-"
        << std::setw(2) << month << "-"
        << std::setw(2) << day << " "
        << std::setw(2) << hour << ":"
        << std::setw(2) << minute << ":"
        << std::setw(2) << second;

    return output.str();
}

static bool parseRealtestDate(
    const std::string& date,
    int& year,
    int& month,
    int& day
) {
    int parsedMonth = 0;
    int parsedDay = 0;
    int parsedYear = 0;

    if (std::sscanf(date.c_str(), "%d/%d/%d", &parsedMonth, &parsedDay, &parsedYear) != 3) {
        return false;
    }

    if (parsedYear < 100) {
        parsedYear += parsedYear >= 70 ? 1900 : 2000;
    }

    if (parsedMonth < 1 || parsedMonth > 12) {
        return false;
    }

    if (parsedDay < 1 || parsedDay > 31) {
        return false;
    }

    year = parsedYear;
    month = parsedMonth;
    day = parsedDay;

    return true;
}

static bool parseRealtestTime(
    const std::string& time,
    int& hour,
    int& minute,
    int& second
) {
    std::string normalized = toLower(trim(time));

    if (normalized.empty() || normalized == "open" || normalized == "intraday") {

        hour = 0;
        minute = 0;
        second = 0;
        return true;
    }

    if (normalized == "close") {
        hour = 23;
        minute = 59;
        second = 59;
        return true;
    }

    int parsedHour = 0;
    int parsedMinute = 0;
    int parsedSecond = 0;

    if (std::sscanf(normalized.c_str(), "%d:%d:%d", &parsedHour, &parsedMinute, &parsedSecond) == 3) {
        // Parsed HH:MM:SS.
    } else if (std::sscanf(normalized.c_str(), "%d:%d", &parsedHour, &parsedMinute) == 2) {
        parsedSecond = 0;
    } else {
        return false;
    }

    if (parsedHour < 0 || parsedHour > 23) {
        return false;
    }

    if (parsedMinute < 0 || parsedMinute > 59) {
        return false;
    }

    if (parsedSecond < 0 || parsedSecond > 59) {
        return false;
    }

    hour = parsedHour;
    minute = parsedMinute;
    second = parsedSecond;

    return true;
}

static bool parseRealtestTimestamp(
    const std::string& date,
    const std::string& time,
    Timestamp& timestamp
) {
    int year = 0;
    int month = 0;
    int day = 0;

    if (!parseRealtestDate(trim(date), year, month, day)) {
        LG_ERROR("Invalid RealTest date: {}", date);
        return false;
    }

    int hour = 0;
    int minute = 0;
    int second = 0;

    if (!parseRealtestTime(trim(time), hour, minute, second)) {
        LG_ERROR("Invalid RealTest time: {}", time);
        return false;
    }

    const std::int64_t days =
        daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));

    const std::int64_t unixSeconds =
        days * 86400LL +
        static_cast<std::int64_t>(hour) * 3600LL +
        static_cast<std::int64_t>(minute) * 60LL +
        static_cast<std::int64_t>(second);

    timestamp = static_cast<Timestamp>(unixSeconds);

    return true;
}

static bool parseRealtestDirection(
    const std::string& side,
    Direction& direction
) {
    const std::string normalized = toLower(trim(side));

    if (normalized == "long") {
        direction = Direction::Long;
        return true;
    }

    if (normalized == "short") {
        direction = Direction::Short;
        return true;
    }

    direction = Direction::Flat;
    return false;
}

static std::string directionToString(Direction direction)
{
    switch (direction) {
        case Direction::Long:
            return "Long";
        case Direction::Short:
            return "Short";
        case Direction::Flat:
            return "Flat";
        default:
            return "Unknown";
    }
}

static bool parseRealtestTradeId(
    const std::string& tradeIdText,
    TradeID& tradeId
) {
    try {
        tradeId = static_cast<TradeID>(std::stoull(trim(tradeIdText)));
        return true;
    } catch (const std::exception& e) {
        LG_ERROR("Invalid RealTest trade id '{}': {}", tradeIdText, e.what());
        return false;
    }
}

static bool convertRealtestTradesToTrades(
    const std::vector<RealtestTrade>& realtestTrades,
    std::vector<Trade>& trades
) {
    trades.clear();
    trades.reserve(realtestTrades.size());

    for (const RealtestTrade& realtestTrade : realtestTrades) {
        Trade trade;

        if (!parseRealtestTradeId(realtestTrade.trade, trade.trade_id_)) {
            return false;
        }

        if (!parseRealtestTimestamp(realtestTrade.dateIn, realtestTrade.timeIn, trade.start_)) {
            LG_ERROR("Failed to parse start timestamp for RealTest trade {}", realtestTrade.trade);
            return false;
        }

        if (!parseRealtestTimestamp(realtestTrade.dateOut, realtestTrade.timeOut, trade.end_)) {
            LG_ERROR("Failed to parse end timestamp for RealTest trade {}", realtestTrade.trade);
            return false;
        }

        if (!parseRealtestDirection(realtestTrade.side, trade.direction_)) {
            LG_ERROR(
                "Invalid RealTest trade side '{}' for trade {}",
                realtestTrade.side,
                realtestTrade.trade
            );
            return false;
        }

        trade.commission_ = 0.0;
        trade.coin_ = realtestTrade.symbol;
        trade.entry_ = realtestTrade.priceIn;
        trade.exit_ = realtestTrade.priceOut;
        trade.size_ = realtestTrade.qtyIn;
        trade.pnl_ = realtestTrade.profit;
        trade.current_price_ = realtestTrade.priceOut;
        trade.exited_ = true;
        trade.isSimulated_ = true;
        trade.strategy_name_ = realtestTrade.strategy;

        trades.push_back(std::move(trade));
    }

    std::sort(
        trades.begin(),
        trades.end(),
        [](const Trade& left, const Trade& right) {
            return left.trade_id_ < right.trade_id_;
        }
    );

    LG_INFO("Converted {} RealTest trades into internal Trade objects", trades.size());

    return true;
}

static double percentageDifferenceFromRealtest(
    double realtestValue,
    double backtesterValue
) {
    constexpr double EPSILON = 0.000000001;

    const double denominator = std::fabs(realtestValue);

    if (denominator < EPSILON) {
        return std::fabs(backtesterValue) < EPSILON ? 0.0 : 1000000.0;
    }

    return std::fabs(backtesterValue - realtestValue) / denominator * 100.0;
}

static double dynamicTolerancePercentForTrade(const Trade& realtestTrade)
{
    const double tradeId = static_cast<double>(realtestTrade.trade_id_);

    const double tolerance =
        BASE_DYNAMIC_TOLERANCE_PERCENT +
        tradeId * EXTRA_DYNAMIC_TOLERANCE_PER_TRADE_ID;

    return std::min(tolerance, MAX_DYNAMIC_TOLERANCE_PERCENT);
}

static bool equalWithinDynamicTolerance(
    const Trade& realtestTrade,
    double realtestValue,
    double backtesterValue
) {
    return percentageDifferenceFromRealtest(realtestValue, backtesterValue) <=
           dynamicTolerancePercentForTrade(realtestTrade);
}

static bool sameEntryDateAndSymbol(
    const Trade& realtestTrade,
    const Trade& backtesterTrade
) {
    return
        normalizeTimestamp(realtestTrade.start_) == normalizeTimestamp(backtesterTrade.start_) &&
        realtestTrade.coin_ == backtesterTrade.coin_;
}

static bool tradesMatchManualChecks(
    const Trade& realtestTrade,
    const Trade& backtesterTrade
) {
    const Timestamp realtestStart = normalizeTimestamp(realtestTrade.start_);
    const Timestamp backtesterStart = normalizeTimestamp(backtesterTrade.start_);

    const Timestamp realtestEnd = normalizeTimestamp(realtestTrade.end_);
    const Timestamp backtesterEnd = normalizeTimestamp(backtesterTrade.end_);

    return
        realtestTrade.coin_ == backtesterTrade.coin_ &&
        realtestStart == backtesterStart &&
        realtestEnd == backtesterEnd &&

        equalWithinDynamicTolerance(
            realtestTrade,
            realtestTrade.entry_,
            backtesterTrade.entry_
        ) &&

        equalWithinDynamicTolerance(
            realtestTrade,
            realtestTrade.exit_,
            backtesterTrade.exit_
        ) &&

        equalWithinDynamicTolerance(
            realtestTrade,
            realtestTrade.size_,
            backtesterTrade.size_
        ) &&

        equalWithinDynamicTolerance(
            realtestTrade,
            realtestTrade.pnl_,
            backtesterTrade.pnl_
        );
}

static std::string doubleToString(double value)
{
    std::ostringstream output;
    output << std::fixed << std::setprecision(10) << value;
    return output.str();
}

static std::string boolToString(bool value)
{
    return value ? "YES" : "NO";
}

static std::string csvEscape(const std::string& value)
{
    bool needsQuotes = false;

    for (char c : value) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') {
            needsQuotes = true;
            break;
        }
    }

    if (!needsQuotes) {
        return value;
    }

    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');

    for (char c : value) {
        if (c == '"') {
            escaped += "\"\"";
        } else {
            escaped.push_back(c);
        }
    }

    escaped.push_back('"');
    return escaped;
}

static void writeCsvRow(
    std::ofstream& csv,
    const std::vector<std::string>& fields
) {
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) {
            csv << ",";
        }

        csv << csvEscape(fields[i]);
    }

    csv << "\n";
}

static void appendTradeFields(
    std::vector<std::string>& fields,
    const Trade* trade,
    TradeID displayedTradeId
) {
    if (trade == nullptr) {
        for (int i = 0; i < 12; ++i) {
            fields.emplace_back("");
        }

        return;
    }

    fields.push_back(std::to_string(displayedTradeId));
    fields.push_back(trade->coin_);
    fields.push_back(directionToString(trade->direction_));
    fields.push_back(timestampToString(trade->start_));
    fields.push_back(std::to_string(trade->start_));
    fields.push_back(timestampToString(trade->end_));
    fields.push_back(std::to_string(trade->end_));
    fields.push_back(doubleToString(trade->entry_));
    fields.push_back(doubleToString(trade->exit_));
    fields.push_back(doubleToString(trade->size_));
    fields.push_back(doubleToString(trade->pnl_));
    fields.push_back(boolToString(trade->exited_));
}

static void writeMismatchCsvHeader(std::ofstream& csv)
{
    writeCsvRow(
        csv,
        {
            "mismatch_type",
            "comparison_index",

            "same_symbol",
            "same_start",
            "same_end",
            "entry_ok",
            "exit_ok",
            "size_ok",
            "pnl_ok",

            "entry_diff_pct",
            "exit_diff_pct",
            "size_diff_pct",
            "pnl_diff_pct",
            "dynamic_tolerance_pct",

            "realtest_trade_id",
            "realtest_symbol",
            "realtest_side",
            "realtest_start",
            "realtest_start_raw",
            "realtest_end",
            "realtest_end_raw",
            "realtest_entry",
            "realtest_exit",
            "realtest_size",
            "realtest_pnl",
            "realtest_exited",

            "backtester_trade_id",
            "backtester_symbol",
            "backtester_side",
            "backtester_start",
            "backtester_start_raw",
            "backtester_end",
            "backtester_end_raw",
            "backtester_entry",
            "backtester_exit",
            "backtester_size",
            "backtester_pnl",
            "backtester_exited"
        }
    );
}

static void writeMismatchCsvRow(
    std::ofstream& csv,
    const std::string& mismatchType,
    std::size_t comparisonIndex,
    const Trade* realtestTrade,
    const Trade* backtesterTrade,
    TradeID backtesterTradeId
) {
    bool sameSymbol = false;
    bool sameStart = false;
    bool sameEnd = false;
    bool entryOk = false;
    bool exitOk = false;
    bool sizeOk = false;
    bool pnlOk = false;

    std::string entryDiffPct;
    std::string exitDiffPct;
    std::string sizeDiffPct;
    std::string pnlDiffPct;
    std::string tolerancePct;

    if (realtestTrade != nullptr) {
        tolerancePct = doubleToString(dynamicTolerancePercentForTrade(*realtestTrade));
    }

    if (realtestTrade != nullptr && backtesterTrade != nullptr) {
        const Timestamp realtestStart = normalizeTimestamp(realtestTrade->start_);
        const Timestamp backtesterStart = normalizeTimestamp(backtesterTrade->start_);

        const Timestamp realtestEnd = normalizeTimestamp(realtestTrade->end_);
        const Timestamp backtesterEnd = normalizeTimestamp(backtesterTrade->end_);

        sameSymbol = realtestTrade->coin_ == backtesterTrade->coin_;
        sameStart = realtestStart == backtesterStart;
        sameEnd = realtestEnd == backtesterEnd;

        entryOk = equalWithinDynamicTolerance(
            *realtestTrade,
            realtestTrade->entry_,
            backtesterTrade->entry_
        );

        exitOk = equalWithinDynamicTolerance(
            *realtestTrade,
            realtestTrade->exit_,
            backtesterTrade->exit_
        );

        sizeOk = equalWithinDynamicTolerance(
            *realtestTrade,
            realtestTrade->size_,
            backtesterTrade->size_
        );

        pnlOk = equalWithinDynamicTolerance(
            *realtestTrade,
            realtestTrade->pnl_,
            backtesterTrade->pnl_
        );

        entryDiffPct = doubleToString(
            percentageDifferenceFromRealtest(
                realtestTrade->entry_,
                backtesterTrade->entry_
            )
        );

        exitDiffPct = doubleToString(
            percentageDifferenceFromRealtest(
                realtestTrade->exit_,
                backtesterTrade->exit_
            )
        );

        sizeDiffPct = doubleToString(
            percentageDifferenceFromRealtest(
                realtestTrade->size_,
                backtesterTrade->size_
            )
        );

        pnlDiffPct = doubleToString(
            percentageDifferenceFromRealtest(
                realtestTrade->pnl_,
                backtesterTrade->pnl_
            )
        );
    }

    std::vector<std::string> fields;

    fields.push_back(mismatchType);
    fields.push_back(std::to_string(comparisonIndex));

    fields.push_back(boolToString(sameSymbol));
    fields.push_back(boolToString(sameStart));
    fields.push_back(boolToString(sameEnd));
    fields.push_back(boolToString(entryOk));
    fields.push_back(boolToString(exitOk));
    fields.push_back(boolToString(sizeOk));
    fields.push_back(boolToString(pnlOk));

    fields.push_back(entryDiffPct);
    fields.push_back(exitDiffPct);
    fields.push_back(sizeDiffPct);
    fields.push_back(pnlDiffPct);
    fields.push_back(tolerancePct);

    appendTradeFields(
        fields,
        realtestTrade,
        realtestTrade != nullptr ? realtestTrade->trade_id_ : static_cast<TradeID>(0)
    );

    appendTradeFields(
        fields,
        backtesterTrade,
        backtesterTrade != nullptr ? backtesterTradeId : static_cast<TradeID>(0)
    );

    writeCsvRow(csv, fields);
}


static std::int64_t entryDayKey(const Trade& trade)
{
    Timestamp normalized = normalizeTimestamp(trade.start_);
    std::int64_t seconds = static_cast<std::int64_t>(normalized);

    if (seconds < 0) {
        return (seconds - 86399LL) / 86400LL;
    }

    return seconds / 86400LL;
}

static bool sameEntryDay(const Trade& left, const Trade& right)
{
    return entryDayKey(left) == entryDayKey(right);
}

static void printTerminalTrade(
    const std::string& label,
    const Trade* trade,
    TradeID displayedTradeId
) {
    std::cout << "\n" << label << "\n";

    if (trade == nullptr) {
        std::cout << "  <no trade>\n";
        return;
    }

    std::cout << "  id        : " << displayedTradeId << "\n";
    std::cout << "  coin      : " << trade->coin_ << "\n";
    std::cout << "  direction : " << directionToString(trade->direction_) << "\n";
    std::cout << "  start     : " << timestampToString(trade->start_) << "\n";
    std::cout << "  end       : " << timestampToString(trade->end_) << "\n";
    std::cout << "  entry     : " << doubleToString(trade->entry_) << "\n";
    std::cout << "  exit      : " << doubleToString(trade->exit_) << "\n";
    std::cout << "  size      : " << doubleToString(trade->size_) << "\n";
    std::cout << "  pnl       : " << doubleToString(trade->pnl_) << "\n";
    std::cout << "  exited    : " << boolToString(trade->exited_) << "\n";
}

static void printNumericDifference(
    const std::string& name,
    double realtestValue,
    double backtesterValue
) {
    const double rawDifference = backtesterValue - realtestValue;
    const double diffPct = percentageDifferenceFromRealtest(realtestValue, backtesterValue);

    std::cout << "  " << name << "\n";
    std::cout << "    RealTest   : " << doubleToString(realtestValue) << "\n";
    std::cout << "    Backtester : " << doubleToString(backtesterValue) << "\n";
    std::cout << "    Difference : " << doubleToString(rawDifference) << "\n";
    std::cout << "    Diff %     : " << doubleToString(diffPct) << "%\n";
}

static bool waitForEnterOrQuit()
{
    std::cout << "\nPress ENTER for next comparison, or type q + ENTER to quit: ";

    std::string input;
    std::getline(std::cin, input);

    return toLower(trim(input)) == "q";
}

static bool compareTradeVectorsInteractive(
    const std::vector<Trade>& referenceTrades,
    const std::map<TradeID, Trade>& candidateTradesById
) {
    struct CandidateTrade {
        TradeID tradeId;
        const Trade* trade;
    };

    std::vector<const Trade*> realtestTradesSorted;
    realtestTradesSorted.reserve(referenceTrades.size());

    for (const Trade& trade : referenceTrades) {
        realtestTradesSorted.push_back(&trade);
    }

    std::sort(
        realtestTradesSorted.begin(),
        realtestTradesSorted.end(),
        [](const Trade* left, const Trade* right) {
            if (entryDayKey(*left) != entryDayKey(*right)) {
                return entryDayKey(*left) < entryDayKey(*right);
            }

            if (left->coin_ != right->coin_) {
                return left->coin_ < right->coin_;
            }

            return left->trade_id_ < right->trade_id_;
        }
    );

    std::vector<CandidateTrade> backtesterTrades;
    backtesterTrades.reserve(candidateTradesById.size());

    for (const auto& entry : candidateTradesById) {
        backtesterTrades.push_back(CandidateTrade{entry.first, &entry.second});
    }

    std::sort(
        backtesterTrades.begin(),
        backtesterTrades.end(),
        [](const CandidateTrade& left, const CandidateTrade& right) {
            if (entryDayKey(*left.trade) != entryDayKey(*right.trade)) {
                return entryDayKey(*left.trade) < entryDayKey(*right.trade);
            }

            if (left.trade->coin_ != right.trade->coin_) {
                return left.trade->coin_ < right.trade->coin_;
            }

            return left.tradeId < right.tradeId;
        }
    );

    std::vector<bool> backtesterMatched(backtesterTrades.size(), false);

    std::size_t fullyMatchedCount = 0;
    std::size_t differentCount = 0;
    std::size_t missingBacktesterCount = 0;
    std::size_t fallbackSameDayCount = 0;
    std::size_t comparisonIndex = 0;

    std::cout << "\n============================================================\n";
    std::cout << "INTERACTIVE TRADE COMPARISON\n";
    std::cout << "============================================================\n";
    std::cout << "Matching rule:\n";
    std::cout << "  1. Match only exact start date/time + same coin.\n";
    std::cout << "  2. If not found, show <no trade>.\n";
    std::cout << "  3. Never match a different coin as fallback.\n";

    for (const Trade* realtestTrade : realtestTradesSorted) {
        ++comparisonIndex;

        std::size_t chosenBacktesterIndex = backtesterTrades.size();
        std::string matchType = "NO_BACKTESTER_TRADE";

        // Only match exact start date/time + same coin.
        // Do NOT fallback to another coin from the same day.
        for (std::size_t i = 0; i < backtesterTrades.size(); ++i) {
            if (backtesterMatched[i]) {
                continue;
            }

            const Trade* backtesterTrade = backtesterTrades[i].trade;

            if (sameEntryDateAndSymbol(*realtestTrade, *backtesterTrade)) {
                chosenBacktesterIndex = i;
                matchType = "EXACT_START_AND_COIN";
                break;
            }
        }

        const Trade* backtesterTrade = nullptr;
        TradeID backtesterTradeId = static_cast<TradeID>(0);

        if (chosenBacktesterIndex != backtesterTrades.size()) {
            backtesterMatched[chosenBacktesterIndex] = true;
            backtesterTrade = backtesterTrades[chosenBacktesterIndex].trade;
            backtesterTradeId = backtesterTrades[chosenBacktesterIndex].tradeId;
        } else {
            ++missingBacktesterCount;
        }

        const bool fullMatch =
            backtesterTrade != nullptr &&
            tradesMatchManualChecks(*realtestTrade, *backtesterTrade);

        if (fullMatch) {
            ++fullyMatchedCount;
            continue;   // skip printing matched trades
        }

        ++differentCount;

        std::cout << "\n\n============================================================\n";
        std::cout << "MISMATCH #" << differentCount << "\n";
        std::cout << "REAL COMPARISON INDEX: " << comparisonIndex << "\n";
        std::cout << "MATCH TYPE  : " << matchType << "\n";
        std::cout << "FULL MATCH  : " << boolToString(fullMatch) << "\n";
        std::cout << "============================================================\n";

        printTerminalTrade(
            "REALTEST",
            realtestTrade,
            realtestTrade->trade_id_
        );

        printTerminalTrade(
            "BACKTESTER",
            backtesterTrade,
            backtesterTradeId
        );

        if (backtesterTrade != nullptr) {
            std::cout << "\nDIFFERENCES\n";

            std::cout << "  same coin  : "
                      << boolToString(realtestTrade->coin_ == backtesterTrade->coin_)
                      << "\n";

            std::cout << "  same day   : "
                      << boolToString(sameEntryDay(*realtestTrade, *backtesterTrade))
                      << "\n";

            std::cout << "  same start : "
                      << boolToString(
                          normalizeTimestamp(realtestTrade->start_) ==
                          normalizeTimestamp(backtesterTrade->start_)
                      )
                      << "\n";

            std::cout << "  same end   : "
                      << boolToString(
                          normalizeTimestamp(realtestTrade->end_) ==
                          normalizeTimestamp(backtesterTrade->end_)
                      )
                      << "\n";

            printNumericDifference(
                "entry",
                realtestTrade->entry_,
                backtesterTrade->entry_
            );

            printNumericDifference(
                "exit",
                realtestTrade->exit_,
                backtesterTrade->exit_
            );

            printNumericDifference(
                "size",
                realtestTrade->size_,
                backtesterTrade->size_
            );

            printNumericDifference(
                "pnl",
                realtestTrade->pnl_,
                backtesterTrade->pnl_
            );
        }

        if (waitForEnterOrQuit()) {
            break;
        }
    }

    for (std::size_t i = 0; i < backtesterTrades.size(); ++i) {
        if (backtesterMatched[i]) {
            continue;
        }

        ++comparisonIndex;

        std::cout << "\n\n============================================================\n";
        std::cout << "UNMATCHED BACKTESTER TRADE #" << comparisonIndex << "\n";
        std::cout << "============================================================\n";

        printTerminalTrade(
            "REALTEST",
            nullptr,
            static_cast<TradeID>(0)
        );

        printTerminalTrade(
            "BACKTESTER",
            backtesterTrades[i].trade,
            backtesterTrades[i].tradeId
        );

        if (waitForEnterOrQuit()) {
            break;
        }
    }

    std::cout << "\n============================================================\n";
    std::cout << "INTERACTIVE COMPARISON SUMMARY\n";
    std::cout << "============================================================\n";
    std::cout << "RealTest trades checked        : " << realtestTradesSorted.size() << "\n";
    std::cout << "Backtester trades total        : " << backtesterTrades.size() << "\n";
    std::cout << "Fully matched                  : " << fullyMatchedCount << "\n";
    std::cout << "Different / missing            : " << differentCount << "\n";
    std::cout << "Missing backtester same-day    : " << missingBacktesterCount << "\n";

    return differentCount == 0 && missingBacktesterCount == 0;
}

static bool compareTradeVectorsIgnoringId(
    const std::vector<Trade>& referenceTrades,
    const std::map<TradeID, Trade>& candidateTradesById,
    bool showAllTrades,
    const std::filesystem::path& mismatchesCsvPath
) {
    (void)showAllTrades;

    struct CandidateTrade {
        TradeID tradeId;
        const Trade* trade;
    };

    std::vector<const Trade*> realtestTradesSorted;
    realtestTradesSorted.reserve(referenceTrades.size());

    for (const Trade& trade : referenceTrades) {
        realtestTradesSorted.push_back(&trade);
    }

    std::sort(
        realtestTradesSorted.begin(),
        realtestTradesSorted.end(),
        [](const Trade* left, const Trade* right) {
            if (normalizeTimestamp(left->start_) != normalizeTimestamp(right->start_)) {
                return normalizeTimestamp(left->start_) < normalizeTimestamp(right->start_);
            }

            if (left->coin_ != right->coin_) {
                return left->coin_ < right->coin_;
            }

            return left->trade_id_ < right->trade_id_;
        }
    );

    std::vector<CandidateTrade> backtesterTrades;
    backtesterTrades.reserve(candidateTradesById.size());

    for (const auto& entry : candidateTradesById) {
        backtesterTrades.push_back(CandidateTrade{entry.first, &entry.second});
    }

    std::sort(
        backtesterTrades.begin(),
        backtesterTrades.end(),
        [](const CandidateTrade& left, const CandidateTrade& right) {
            if (normalizeTimestamp(left.trade->start_) != normalizeTimestamp(right.trade->start_)) {
                return normalizeTimestamp(left.trade->start_) < normalizeTimestamp(right.trade->start_);
            }

            if (left.trade->coin_ != right.trade->coin_) {
                return left.trade->coin_ < right.trade->coin_;
            }

            return left.tradeId < right.tradeId;
        }
    );

    const std::filesystem::path parentPath = mismatchesCsvPath.parent_path();

    if (!parentPath.empty()) {
        std::filesystem::create_directories(parentPath);
    }

    std::ofstream mismatchCsv(mismatchesCsvPath);

    if (!mismatchCsv.is_open()) {
        LG_ERROR("Could not open mismatch CSV for writing: {}", mismatchesCsvPath.string());
        return false;
    }

    writeMismatchCsvHeader(mismatchCsv);

    LG_INFO(
        "Starting trade comparison by entry date and symbol. RealTest trades: {}, backtester trades: {}, mismatchCsv={}",
        realtestTradesSorted.size(),
        backtesterTrades.size(),
        mismatchesCsvPath.string()
    );

    std::vector<bool> backtesterMatched(backtesterTrades.size(), false);

    std::size_t matchedCount = 0;
    std::size_t differentCount = 0;
    std::size_t missingMatchCount = 0;
    std::size_t duplicateMatchCount = 0;
    std::size_t checkedCount = 0;
    std::size_t writtenMismatchRows = 0;

    for (std::size_t realtestIndex = 0; realtestIndex < realtestTradesSorted.size(); ++realtestIndex) {
        const Trade* realtestTrade = realtestTradesSorted[realtestIndex];

        std::vector<std::size_t> matchingBacktesterIndexes;

        for (std::size_t backtesterIndex = 0; backtesterIndex < backtesterTrades.size(); ++backtesterIndex) {
            if (backtesterMatched[backtesterIndex]) {
                continue;
            }

            const Trade* backtesterTrade = backtesterTrades[backtesterIndex].trade;

            if (sameEntryDateAndSymbol(*realtestTrade, *backtesterTrade)) {
                matchingBacktesterIndexes.push_back(backtesterIndex);
            }
        }

        ++checkedCount;

        if (matchingBacktesterIndexes.empty()) {
            ++missingMatchCount;
            ++differentCount;

            writeMismatchCsvRow(
                mismatchCsv,
                "MISSING_BACKTESTER_MATCH",
                realtestIndex + 1,
                realtestTrade,
                nullptr,
                static_cast<TradeID>(0)
            );

            ++writtenMismatchRows;
            continue;
        }

        if (matchingBacktesterIndexes.size() > 1) {
            ++duplicateMatchCount;
        }

        const std::size_t backtesterIndex = matchingBacktesterIndexes.front();
        backtesterMatched[backtesterIndex] = true;

        const Trade* backtesterTrade = backtesterTrades[backtesterIndex].trade;
        const TradeID backtesterTradeId = backtesterTrades[backtesterIndex].tradeId;

        const bool isMatch = tradesMatchManualChecks(*realtestTrade, *backtesterTrade);

        if (isMatch) {
            ++matchedCount;
        } else {
            ++differentCount;

            writeMismatchCsvRow(
                mismatchCsv,
                "DIFFERENT_MATCHED_TRADE",
                realtestIndex + 1,
                realtestTrade,
                backtesterTrade,
                backtesterTradeId
            );

            ++writtenMismatchRows;
        }
    }

    std::size_t unmatchedBacktesterCount = 0;

    for (std::size_t i = 0; i < backtesterMatched.size(); ++i) {
        if (!backtesterMatched[i]) {
            ++unmatchedBacktesterCount;

            writeMismatchCsvRow(
                mismatchCsv,
                "UNMATCHED_BACKTESTER_TRADE",
                i + 1,
                nullptr,
                backtesterTrades[i].trade,
                backtesterTrades[i].tradeId
            );

            ++writtenMismatchRows;
        }
    }

    mismatchCsv.close();

    std::cout << "\n============================================================\n";
    std::cout << "COMPARISON SUMMARY\n";
    std::cout << "============================================================\n";
    std::cout << "Checked RealTest trades           : " << checkedCount << "\n";
    std::cout << "Matched fully                     : " << matchedCount << "\n";
    std::cout << "Different matched trades          : " << differentCount << "\n";
    std::cout << "Missing entry-date+symbol matches : " << missingMatchCount << "\n";
    std::cout << "Duplicate entry-date+symbol hits  : " << duplicateMatchCount << "\n";
    std::cout << "Unmatched backtester trades       : " << unmatchedBacktesterCount << "\n";
    std::cout << "RealTest total                    : " << realtestTradesSorted.size() << "\n";
    std::cout << "Backtester total                  : " << backtesterTrades.size() << "\n";
    std::cout << "Mismatch rows written             : " << writtenMismatchRows << "\n";
    std::cout << "Mismatch CSV                      : " << mismatchesCsvPath.string() << "\n";

    LG_INFO(
        "Trade comparison finished. checked={}, matched={}, different={}, missing_entry_symbol_matches={}, duplicate_hits={}, unmatched_backtester={}, realtest_total={}, backtester_total={}, mismatch_rows_written={}, mismatch_csv={}",
        checkedCount,
        matchedCount,
        differentCount,
        missingMatchCount,
        duplicateMatchCount,
        unmatchedBacktesterCount,
        realtestTradesSorted.size(),
        backtesterTrades.size(),
        writtenMismatchRows,
        mismatchesCsvPath.string()
    );

    return
        differentCount == 0 &&
        missingMatchCount == 0 &&
        duplicateMatchCount == 0 &&
        unmatchedBacktesterCount == 0;
}

bool compareBacktests(
    std::filesystem::path& realtest_trades,
    std::map<TradeID, Trade>& backtesterTradesHistory,
    bool showAllTrades,
    const std::filesystem::path& mismatchesCsvPath
) {
    std::vector<RealtestTrade> realtestCsvTrades;

    if (!loadRealtestTrades(realtest_trades, realtestCsvTrades)) {
        LG_ERROR("Failed to load RealTest trades from {}", realtest_trades.string());
        return false;
    }

    std::vector<Trade> realtestTrades;

    if (!convertRealtestTradesToTrades(realtestCsvTrades, realtestTrades)) {
        LG_ERROR("Failed to convert RealTest trades into internal Trade objects");
        return false;
    }

    LG_INFO(
        "Starting comparison. RealTest trades: {}, backtester trades: {}, showAllTrades={}, mismatchCsv={}",
        realtestTrades.size(),
        backtesterTradesHistory.size(),
        showAllTrades,
        mismatchesCsvPath.string()
    );

    // TERMINAL MODE ONLY:
    // Ignore showAllTrades and mismatchesCsvPath here so this function always prints
    // trade-by-trade comparisons in the terminal and waits for ENTER between pairs.
    (void)showAllTrades;
    (void)mismatchesCsvPath;

    return compareTradeVectorsInteractive(
        realtestTrades,
        backtesterTradesHistory
    );
}

bool compareBacktests(
    std::filesystem::path& realtest_trades,
    std::map<TradeID, Trade>& backtesterTradesHistory,
    bool showAllTrades
) {
    std::filesystem::path mismatchesCsvPath =
        realtest_trades.parent_path() / "realtest_backtester_mismatches.csv";

    return compareBacktests(
        realtest_trades,
        backtesterTradesHistory,
        showAllTrades,
        mismatchesCsvPath
    );
}

bool compareBacktests(
    std::filesystem::path& realtest_trades,
    std::map<TradeID, Trade>& backtesterTradesHistory
) {
    // Default to interactive terminal comparison.
    return compareBacktests(
        realtest_trades,
        backtesterTradesHistory,
        true
    );
}

bool compareBacktestCampaigns(
    std::filesystem::path& realtest_trades,
    std::map<TradeID, Trade>& backtesterTradesHistory,
    const std::filesystem::path& comparisonCsvPath,
    double priceTolerancePercent
)
{
    if (!std::isfinite(priceTolerancePercent) || priceTolerancePercent < 0.0)
        throw std::invalid_argument("Campaign comparison price tolerance must be non-negative");

    std::vector<RealtestTrade> realtestCsvTrades;
    if (!loadRealtestTrades(realtest_trades, realtestCsvTrades))
        return false;

    std::vector<Trade> realtestTrades;
    if (!convertRealtestTradesToTrades(realtestCsvTrades, realtestTrades))
        return false;

    std::vector<std::pair<TradeID, const Trade*>> backtesterTrades;
    backtesterTrades.reserve(backtesterTradesHistory.size());
    for (const auto& [tradeId, trade] : backtesterTradesHistory)
        backtesterTrades.push_back({tradeId, &trade});

    std::vector<bool> backtesterMatched(backtesterTrades.size(), false);

    if (!comparisonCsvPath.parent_path().empty())
        std::filesystem::create_directories(comparisonCsvPath.parent_path());

    std::ofstream csv(comparisonCsvPath);
    if (!csv.is_open())
        throw std::runtime_error("Could not create campaign comparison CSV");

    csv
        << "realtest_trade_id,backtester_trade_id,coin,direction_match,entry_date_match,"
        << "exit_date_match,realtest_entry_price,backtester_entry_price,entry_price_match,"
        << "realtest_exit_price,backtester_exit_price,exit_price_match,"
        << "realtest_pnl_sign,backtester_pnl_sign,pnl_sign_match,campaign_match\n";

    auto pnlSign = [](double value) {
        if (value > 0.0) return 1;
        if (value < 0.0) return -1;
        return 0;
    };

    std::size_t matchedCampaigns = 0;
    std::size_t missingCampaigns = 0;
    std::size_t differentCampaigns = 0;
    std::size_t pnlSignMatches = 0;
    std::size_t comparedPnlSigns = 0;
    std::size_t comparisonIndex = 0;
    std::size_t displayedMismatchIndex = 0;
    bool interactiveQuit = false;

    std::cout << "\n============================================================\n";
    std::cout << "INTERACTIVE VOL-TARGET CAMPAIGN COMPARISON\n";
    std::cout << "============================================================\n";
    std::cout << "Validation checks : direction, entry/exit dates and prices\n";
    std::cout << "Informational     : size, PnL magnitude and PnL sign\n";
    std::cout << "Matched campaigns are skipped; mismatches are shown one by one.\n";
    std::cout << "Price tolerance   : " << priceTolerancePercent << "%\n";

    for (const Trade& realtestTrade : realtestTrades) {
        ++comparisonIndex;
        std::size_t matchIndex = backtesterTrades.size();

        for (std::size_t i = 0; i < backtesterTrades.size(); ++i) {
            if (backtesterMatched[i])
                continue;

            const Trade& candidate = *backtesterTrades[i].second;
            if (candidate.coin_ == realtestTrade.coin_ &&
                normalizeTimestamp(candidate.start_) == normalizeTimestamp(realtestTrade.start_)) {
                matchIndex = i;
                break;
            }
        }

        if (matchIndex == backtesterTrades.size()) {
            ++missingCampaigns;
            csv
                << realtestTrade.trade_id_ << ",," << csvEscape(realtestTrade.coin_)
                << ",0,0,0," << realtestTrade.entry_ << ",,0,"
                << realtestTrade.exit_ << ",,0,"
                << pnlSign(realtestTrade.pnl_) << ",,0,0\n";

            if (!interactiveQuit) {
                ++displayedMismatchIndex;
                std::cout << "\n\n============================================================\n";
                std::cout << "VOL-TARGET MISMATCH #" << displayedMismatchIndex << "\n";
                std::cout << "REAL COMPARISON INDEX: " << comparisonIndex << "\n";
                std::cout << "MATCH TYPE  : NO_BACKTESTER_CAMPAIGN\n";
                std::cout << "CAMPAIGN OK : false\n";
                std::cout << "============================================================\n";
                printTerminalTrade("REALTEST", &realtestTrade, realtestTrade.trade_id_);
                printTerminalTrade("BACKTESTER", nullptr, static_cast<TradeID>(0));
                interactiveQuit = waitForEnterOrQuit();
            }
            continue;
        }

        backtesterMatched[matchIndex] = true;
        const TradeID backtesterTradeId = backtesterTrades[matchIndex].first;
        const Trade& backtesterTrade = *backtesterTrades[matchIndex].second;

        const bool directionMatch = realtestTrade.direction_ == backtesterTrade.direction_;
        const bool entryDateMatch =
            normalizeTimestamp(realtestTrade.start_) == normalizeTimestamp(backtesterTrade.start_);
        const bool exitDateMatch =
            normalizeTimestamp(realtestTrade.end_) == normalizeTimestamp(backtesterTrade.end_);
        const bool entryPriceMatch =
            percentageDifferenceFromRealtest(realtestTrade.entry_, backtesterTrade.entry_) <=
            priceTolerancePercent;
        const bool exitPriceMatch =
            percentageDifferenceFromRealtest(realtestTrade.exit_, backtesterTrade.exit_) <=
            priceTolerancePercent;

        const int realtestPnlSign = pnlSign(realtestTrade.pnl_);
        const int backtesterPnlSign = pnlSign(backtesterTrade.pnl_);
        const bool pnlSignMatch = realtestPnlSign == backtesterPnlSign;
        ++comparedPnlSigns;
        if (pnlSignMatch)
            ++pnlSignMatches;

        const bool campaignMatch =
            directionMatch && entryDateMatch && exitDateMatch && entryPriceMatch && exitPriceMatch;

        if (campaignMatch)
            ++matchedCampaigns;
        else
            ++differentCampaigns;

        csv
            << realtestTrade.trade_id_ << ','
            << backtesterTradeId << ','
            << csvEscape(realtestTrade.coin_) << ','
            << (directionMatch ? 1 : 0) << ','
            << (entryDateMatch ? 1 : 0) << ','
            << (exitDateMatch ? 1 : 0) << ','
            << realtestTrade.entry_ << ','
            << backtesterTrade.entry_ << ','
            << (entryPriceMatch ? 1 : 0) << ','
            << realtestTrade.exit_ << ','
            << backtesterTrade.exit_ << ','
            << (exitPriceMatch ? 1 : 0) << ','
            << realtestPnlSign << ','
            << backtesterPnlSign << ','
            << (pnlSignMatch ? 1 : 0) << ','
            << (campaignMatch ? 1 : 0)
            << '\n';

        if (campaignMatch || interactiveQuit)
            continue;

        ++displayedMismatchIndex;
        std::cout << "\n\n============================================================\n";
        std::cout << "VOL-TARGET MISMATCH #" << displayedMismatchIndex << "\n";
        std::cout << "REAL COMPARISON INDEX: " << comparisonIndex << "\n";
        std::cout << "MATCH TYPE  : EXACT_START_AND_COIN\n";
        std::cout << "CAMPAIGN OK : false\n";
        std::cout << "============================================================\n";

        printTerminalTrade("REALTEST", &realtestTrade, realtestTrade.trade_id_);
        printTerminalTrade("BACKTESTER", &backtesterTrade, backtesterTradeId);

        std::cout << "\nVALIDATION DIFFERENCES\n";
        std::cout << "  same direction : " << boolToString(directionMatch) << "\n";
        std::cout << "  same start     : " << boolToString(entryDateMatch) << "\n";
        std::cout << "  same end       : " << boolToString(exitDateMatch) << "\n";
        std::cout << "  entry price OK : " << boolToString(entryPriceMatch) << "\n";
        printNumericDifference("entry", realtestTrade.entry_, backtesterTrade.entry_);
        std::cout << "  exit price OK  : " << boolToString(exitPriceMatch) << "\n";
        printNumericDifference("exit", realtestTrade.exit_, backtesterTrade.exit_);

        std::cout << "\nINFORMATIONAL ONLY (does not make campaign fail)\n";
        printNumericDifference("size", realtestTrade.size_, backtesterTrade.size_);
        printNumericDifference("pnl", realtestTrade.pnl_, backtesterTrade.pnl_);
        std::cout << "  PnL sign match : " << boolToString(pnlSignMatch) << "\n";

        interactiveQuit = waitForEnterOrQuit();
    }

    std::size_t unmatchedBacktester = 0;
    for (std::size_t i = 0; i < backtesterMatched.size(); ++i) {
        if (backtesterMatched[i])
            continue;

        ++unmatchedBacktester;
        if (interactiveQuit)
            continue;

        ++displayedMismatchIndex;
        std::cout << "\n\n============================================================\n";
        std::cout << "UNMATCHED BACKTESTER CAMPAIGN #" << displayedMismatchIndex << "\n";
        std::cout << "============================================================\n";
        printTerminalTrade("REALTEST", nullptr, static_cast<TradeID>(0));
        printTerminalTrade(
            "BACKTESTER",
            backtesterTrades[i].second,
            backtesterTrades[i].first
        );
        interactiveQuit = waitForEnterOrQuit();
    }

    std::cout << "\n============================================================\n";
    std::cout << "VOL-TARGET CAMPAIGN VALIDATION SUMMARY\n";
    std::cout << "============================================================\n";
    std::cout << "Campaigns matching date/price/direction : " << matchedCampaigns << "\n";
    std::cout << "Different matched campaigns             : " << differentCampaigns << "\n";
    std::cout << "Missing backtester campaigns            : " << missingCampaigns << "\n";
    std::cout << "Unmatched backtester campaigns          : " << unmatchedBacktester << "\n";
    std::cout << "PnL sign matches (informational only)   : "
              << pnlSignMatches << "/" << comparedPnlSigns << "\n";
    std::cout << "Price tolerance                         : " << priceTolerancePercent << "%\n";
    std::cout << "Comparison CSV                          : " << comparisonCsvPath.string() << "\n";
    if (interactiveQuit)
        std::cout << "Interactive display                     : stopped by user; full CSV/summary still completed\n";

    LG_INFO(
        "Vol-target campaign validation finished. matched={}, different={}, missing={}, unmatched_backtester={}, pnl_sign_matches={}/{}, csv={}",
        matchedCampaigns,
        differentCampaigns,
        missingCampaigns,
        unmatchedBacktester,
        pnlSignMatches,
        comparedPnlSigns,
        comparisonCsvPath.string()
    );

    return differentCampaigns == 0 && missingCampaigns == 0 && unmatchedBacktester == 0;
}

bool compareBacktestBySizing(
    PortfolioSizerKind sizingKind,
    std::filesystem::path& realtest_trades,
    std::map<TradeID, Trade>& backtesterTradesHistory,
    const std::filesystem::path& comparisonCsvPath
)
{
    std::cout << "\n============================================================\n";
    std::cout << "REALTEST VALIDATION MODE\n";
    std::cout << "============================================================\n";
    std::cout << "Portfolio sizer : " << portfolioSizerKindName(sizingKind) << "\n";

    switch (sizingKind) {
        case PortfolioSizerKind::EqualWeight:
            std::cout << "Comparison      : exhaustive trade comparison\n";
            std::cout << "Checks          : coin, entry/exit dates, prices, quantity and PnL\n";
            return compareBacktests(
                realtest_trades,
                backtesterTradesHistory,
                false,
                comparisonCsvPath
            );

        case PortfolioSizerKind::VolatilityTarget:
            std::cout << "Comparison      : strategic campaign comparison\n";
            std::cout << "Checks          : direction, entry/exit dates and prices\n";
            std::cout << "Informational   : PnL sign; quantity/PnL magnitude intentionally ignored\n";
            return compareBacktestCampaigns(
                realtest_trades,
                backtesterTradesHistory,
                comparisonCsvPath
            );

        default:
            LG_ERROR(
                "No RealTest comparison policy defined for portfolio sizer {}",
                portfolioSizerKindName(sizingKind)
            );
            return false;
    }
}

