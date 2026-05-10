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
constexpr double BASE_DYNAMIC_TOLERANCE_PERCENT = 5.0;
constexpr double EXTRA_DYNAMIC_TOLERANCE_PER_TRADE_ID = 0.15;
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

static std::string trim(std::string s) {
    auto notSpace = [](unsigned char c) {
        return !std::isspace(c);
    };

    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());

    return s;
}

static std::string toLower(std::string value) {
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

static std::vector<std::string> parseCsvLine(const std::string& line) {
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

static double parseRealtestNumber(std::string value) {
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

static int parseRealtestInt(std::string value) {
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

static std::int64_t daysFromCivil(int year, unsigned month, unsigned day) {
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

static bool isPackedYYYYMMDD(std::int64_t value) {
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

static Timestamp normalizeTimestamp(Timestamp timestamp) {
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

static std::string timestampToString(Timestamp timestamp) {
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

    if (normalized.empty() || normalized == "open") {
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

static std::string directionToString(Direction direction) {
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

static double dynamicTolerancePercentForTrade(const Trade& realtestTrade) {
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

static bool waitForEnterOrQuit() {
    std::string input;

    std::cout << "\nPress ENTER to continue, or type q then ENTER to stop: ";
    std::getline(std::cin, input);

    return input != "q" && input != "Q";
}

static void printTradeSideBySide(
    std::size_t index,
    const Trade* realtestTrade,
    const Trade* backtesterTrade,
    TradeID backtesterTradeId,
    bool isMatch
) {
    std::cout << "\n============================================================\n";
    std::cout << (isMatch ? "MATCH #" : "DIFFERENT MATCH #") << index + 1 << "\n";
    std::cout << "============================================================\n";

    if (realtestTrade != nullptr) {
        std::cout << "REALTEST\n";
        std::cout << "  trade_id : " << realtestTrade->trade_id_ << "\n";
        std::cout << "  symbol   : " << realtestTrade->coin_ << "\n";
        std::cout << "  side     : " << directionToString(realtestTrade->direction_) << "\n";
        std::cout << "  start    : " << timestampToString(realtestTrade->start_)
                  << "  raw=" << realtestTrade->start_ << "\n";
        std::cout << "  end      : " << timestampToString(realtestTrade->end_)
                  << "  raw=" << realtestTrade->end_ << "\n";
        std::cout << std::fixed << std::setprecision(10);
        std::cout << "  entry    : " << realtestTrade->entry_ << "\n";
        std::cout << "  exit     : " << realtestTrade->exit_ << "\n";
        std::cout << "  size     : " << realtestTrade->size_ << "\n";
        std::cout << "  pnl      : " << realtestTrade->pnl_ << "\n";
    } else {
        std::cout << "REALTEST\n";
        std::cout << "  <missing>\n";
    }

    std::cout << "\n";

    if (backtesterTrade != nullptr) {
        std::cout << "BACKTESTER MATCHED BY entry-date + symbol\n";
        std::cout << "  trade_id : " << backtesterTradeId << "\n";
        std::cout << "  symbol   : " << backtesterTrade->coin_ << "\n";
        std::cout << "  side     : " << directionToString(backtesterTrade->direction_) << "\n";
        std::cout << "  start    : " << timestampToString(backtesterTrade->start_)
                  << "  raw=" << backtesterTrade->start_ << "\n";
        std::cout << "  end      : " << timestampToString(backtesterTrade->end_)
                  << "  raw=" << backtesterTrade->end_ << "\n";
        std::cout << std::fixed << std::setprecision(10);
        std::cout << "  entry    : " << backtesterTrade->entry_ << "\n";
        std::cout << "  exit     : " << backtesterTrade->exit_ << "\n";
        std::cout << "  size     : " << backtesterTrade->size_ << "\n";
        std::cout << "  pnl      : " << backtesterTrade->pnl_ << "\n";
    } else {
        std::cout << "BACKTESTER MATCHED BY entry-date + symbol\n";
        std::cout << "  <missing>\n";
    }

    std::cout << "\n";

    if (realtestTrade != nullptr && backtesterTrade != nullptr) {
        const Timestamp realtestStart = normalizeTimestamp(realtestTrade->start_);
        const Timestamp backtesterStart = normalizeTimestamp(backtesterTrade->start_);

        const Timestamp realtestEnd = normalizeTimestamp(realtestTrade->end_);
        const Timestamp backtesterEnd = normalizeTimestamp(backtesterTrade->end_);

        const bool sameSymbol = realtestTrade->coin_ == backtesterTrade->coin_;
        const bool sameStart = realtestStart == backtesterStart;
        const bool sameEnd = realtestEnd == backtesterEnd;

        const double dynamicTolerancePct =
            dynamicTolerancePercentForTrade(*realtestTrade);

        const bool sameEntry = equalWithinDynamicTolerance(
            *realtestTrade,
            realtestTrade->entry_,
            backtesterTrade->entry_
        );

        const bool sameExit = equalWithinDynamicTolerance(
            *realtestTrade,
            realtestTrade->exit_,
            backtesterTrade->exit_
        );

        const bool sameSize = equalWithinDynamicTolerance(
            *realtestTrade,
            realtestTrade->size_,
            backtesterTrade->size_
        );

        const bool samePnl = equalWithinDynamicTolerance(
            *realtestTrade,
            realtestTrade->pnl_,
            backtesterTrade->pnl_
        );

        const double entryDiffPct = percentageDifferenceFromRealtest(
            realtestTrade->entry_,
            backtesterTrade->entry_
        );

        const double exitDiffPct = percentageDifferenceFromRealtest(
            realtestTrade->exit_,
            backtesterTrade->exit_
        );

        const double sizeDiffPct = percentageDifferenceFromRealtest(
            realtestTrade->size_,
            backtesterTrade->size_
        );

        const double pnlDiffPct = percentageDifferenceFromRealtest(
            realtestTrade->pnl_,
            backtesterTrade->pnl_
        );

        std::cout << "CHECKS\n";

        std::cout << "  symbol same            : " << (sameSymbol ? "YES" : "NO")
                  << "  realtest=" << realtestTrade->coin_
                  << " backtester=" << backtesterTrade->coin_ << "\n";

        std::cout << "  start same             : " << (sameStart ? "YES" : "NO")
                  << "  realtest=" << timestampToString(realtestTrade->start_)
                  << " backtester=" << timestampToString(backtesterTrade->start_) << "\n";

        std::cout << "  end same               : " << (sameEnd ? "YES" : "NO")
                  << "  realtest=" << timestampToString(realtestTrade->end_)
                  << " backtester=" << timestampToString(backtesterTrade->end_) << "\n";

        std::cout << std::fixed << std::setprecision(4);
        std::cout << "  entry within dynamic % : " << (sameEntry ? "YES" : "NO")
                  << "  tolerance_pct=" << dynamicTolerancePct << "%"
                  << " realtest=" << realtestTrade->entry_
                  << " backtester=" << backtesterTrade->entry_
                  << " diff_pct=" << entryDiffPct << "%\n";

        std::cout << std::fixed << std::setprecision(4);
        std::cout << "  exit within dynamic %  : " << (sameExit ? "YES" : "NO")
                  << "  tolerance_pct=" << dynamicTolerancePct << "%"
                  << " realtest=" << realtestTrade->exit_
                  << " backtester=" << backtesterTrade->exit_
                  << " diff_pct=" << exitDiffPct << "%\n";

        std::cout << std::fixed << std::setprecision(4);
        std::cout << "  size within dynamic %  : " << (sameSize ? "YES" : "NO")
                  << "  tolerance_pct=" << dynamicTolerancePct << "%"
                  << " realtest=" << realtestTrade->size_
                  << " backtester=" << backtesterTrade->size_
                  << " diff_pct=" << sizeDiffPct << "%\n";

        std::cout << std::fixed << std::setprecision(4);
        std::cout << "  pnl within dynamic %   : " << (samePnl ? "YES" : "NO")
                  << "  tolerance_pct=" << dynamicTolerancePct << "%"
                  << " realtest=" << realtestTrade->pnl_
                  << " backtester=" << backtesterTrade->pnl_
                  << " diff_pct=" << pnlDiffPct << "%\n";

        std::cout << "  RESULT                 : "
                  << (isMatch ? "MATCH" : "DIFFERENT") << "\n";
    } else {
        std::cout << "CHECKS\n";
        std::cout << "  RESULT                 : DIFFERENT, no backtester trade found with same entry date and symbol\n";
    }
}

static bool compareTradeVectorsIgnoringId(
    const std::vector<Trade>& referenceTrades,
    const std::map<TradeID, Trade>& candidateTradesById,
    bool showAllTrades
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

    LG_INFO(
        "Starting trade comparison by entry date and symbol. RealTest trades: {}, backtester trades: {}, showAllTrades={}",
        realtestTradesSorted.size(),
        backtesterTrades.size(),
        showAllTrades
    );

    std::cout << "\nTrade comparison by entry date + symbol\n";
    std::cout << "RealTest trades  : " << realtestTradesSorted.size() << "\n";
    std::cout << "Backtester trades: " << backtesterTrades.size() << "\n";

    if (realtestTradesSorted.size() != backtesterTrades.size()) {
        std::cout << "\nERROR: Trade counts differ. Aborting comparison.\n";
        std::cout << "RealTest total  : " << realtestTradesSorted.size() << "\n";
        std::cout << "Backtester total: " << backtesterTrades.size() << "\n";

        LG_ERROR(
            "Trade count mismatch. RealTest has {}, backtester has {}. Aborting comparison.",
            realtestTradesSorted.size(),
            backtesterTrades.size()
        );

        return false;
    }

    std::cout << "Trade counts match. Starting lookup by same entry date and same symbol.\n";

    if (showAllTrades) {
        std::cout << "Mode: showing ALL trades one by one.\n";
    } else {
        std::cout << "Mode: showing only DIFFERENT trades.\n";
    }

    std::cout << "Exact checks: symbol, entry date, exit date.\n";
    std::cout << "Dynamic checks: entry, exit, size, and pnl within dynamic percentage.\n";
    std::cout << "Dynamic tolerance: base=" << BASE_DYNAMIC_TOLERANCE_PERCENT
              << "% + trade_id*" << EXTRA_DYNAMIC_TOLERANCE_PER_TRADE_ID
              << "%, capped at " << MAX_DYNAMIC_TOLERANCE_PERCENT << "%.\n";
    std::cout << "Press ENTER after each printed trade to continue. Type q then ENTER to stop.\n";

    std::vector<bool> backtesterMatched(backtesterTrades.size(), false);

    std::size_t matchedCount = 0;
    std::size_t differentCount = 0;
    std::size_t missingMatchCount = 0;
    std::size_t duplicateMatchCount = 0;
    std::size_t checkedCount = 0;

    bool stoppedByUser = false;

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

            printTradeSideBySide(
                realtestIndex,
                realtestTrade,
                nullptr,
                static_cast<TradeID>(0),
                false
            );

            if (!waitForEnterOrQuit()) {
                std::cout << "Manual comparison stopped by user.\n";
                stoppedByUser = true;
                break;
            }

            continue;
        }

        if (matchingBacktesterIndexes.size() > 1) {
            ++duplicateMatchCount;

            std::cout << "\nWARNING: More than one unmatched backtester trade found for RealTest trade "
                      << realtestTrade->trade_id_
                      << " with symbol=" << realtestTrade->coin_
                      << " start=" << timestampToString(realtestTrade->start_)
                      << ". Using the first one.\n";
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
        }

        if (showAllTrades || !isMatch) {
            printTradeSideBySide(
                realtestIndex,
                realtestTrade,
                backtesterTrade,
                backtesterTradeId,
                isMatch
            );

            if (!waitForEnterOrQuit()) {
                std::cout << "Manual comparison stopped by user.\n";
                stoppedByUser = true;
                break;
            }
        }
    }

    std::size_t unmatchedBacktesterCount = 0;

    for (std::size_t i = 0; i < backtesterMatched.size(); ++i) {
        if (!backtesterMatched[i]) {
            ++unmatchedBacktesterCount;
        }
    }

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
    std::cout << "Stopped by user                   : " << (stoppedByUser ? "YES" : "NO") << "\n";

    LG_INFO(
        "Trade comparison finished. checked={}, matched={}, different={}, missing_entry_symbol_matches={}, duplicate_hits={}, unmatched_backtester={}, realtest_total={}, backtester_total={}, stopped_by_user={}",
        checkedCount,
        matchedCount,
        differentCount,
        missingMatchCount,
        duplicateMatchCount,
        unmatchedBacktesterCount,
        realtestTradesSorted.size(),
        backtesterTrades.size(),
        stoppedByUser
    );

    return
        !stoppedByUser &&
        differentCount == 0 &&
        missingMatchCount == 0 &&
        duplicateMatchCount == 0 &&
        unmatchedBacktesterCount == 0;
}

bool compareBacktests(
    std::filesystem::path& realtest_trades,
    std::map<TradeID, Trade>& backtesterTradesHistory,
    bool showAllTrades
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
        "Starting comparison. RealTest trades: {}, backtester trades: {}, showAllTrades={}",
        realtestTrades.size(),
        backtesterTradesHistory.size(),
        showAllTrades
    );

    return compareTradeVectorsIgnoringId(
        realtestTrades,
        backtesterTradesHistory,
        showAllTrades
    );
}

bool compareBacktests(
    std::filesystem::path& realtest_trades,
    std::map<TradeID, Trade>& backtesterTradesHistory
) {
    return compareBacktests(
        realtest_trades,
        backtesterTradesHistory,
        false
    );
}