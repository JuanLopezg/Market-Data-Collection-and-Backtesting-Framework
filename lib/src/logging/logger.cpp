#include "logger.h"

#include <chrono>
#include <cmath>
#include <ctime>
#include <iostream>

namespace {

const char* SeverityLabel(LogSeverityLevel severity)
{
    // Fixed width keeps the message column aligned.
    switch (severity) {
        case LogSeverityLevel::DEBUG: return "DEBUG";
        case LogSeverityLevel::INFO:  return "INFO ";
        case LogSeverityLevel::WARN:  return "WARN ";
        case LogSeverityLevel::ERROR: return "ERROR";
        case LogSeverityLevel::ALERT: return "ALERT";
    }
    return "INFO ";
}

std::tm UtcCalendar(std::time_t seconds)
{
    std::tm calendar{};
    gmtime_r(&seconds, &calendar);
    return calendar;
}

std::string RemoveAll(std::string text, std::string_view token)
{
    for (auto position = text.find(token); position != std::string::npos; position = text.find(token))
        text.erase(position, token.size());
    return text;
}

std::string RemoveTemplateArguments(const std::string& text)
{
    std::string result;
    int depth = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        // Keep comparison operators such as "operator<" readable.
        const bool afterOperator = i >= 8 && text.compare(i - 8, 8, "operator") == 0;
        if (c == '<' && !afterOperator) {
            ++depth;
            continue;
        }
        if (c == '>' && depth > 0) {
            --depth;
            continue;
        }
        if (depth == 0)
            result.push_back(c);
    }
    return result;
}

} // namespace

Logger& Logger::Instance()
{
    static Logger instance;
    return instance;
}

void Logger::Setup(bool debugEnabled, bool quiet, const std::string& filePath)
{
    std::lock_guard<std::mutex> lock(mutex_);
    debugEnabled_ = debugEnabled;
    quiet_ = quiet;

    if (file_.is_open())
        file_.close();
    if (!filePath.empty())
        file_.open(filePath, std::ios::out | std::ios::app);
}

bool Logger::IsEnabled(LogSeverityLevel severity) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return severity != LogSeverityLevel::DEBUG || debugEnabled_;
}

void Logger::Write(LogSeverityLevel severity,
                   std::string_view functionName,
                   std::string_view message)
{
    std::string line = CurrentUtcLogTimestamp();
    line += " [";
    line += SeverityLabel(severity);
    line += "] ";
    line += functionName;
    line += " : ";
    // One record per line keeps host log files grep-able and safely trimmable.
    for (const char c : message)
        line.push_back(c == '\n' || c == '\r' ? ' ' : c);
    line += '\n';

    std::lock_guard<std::mutex> lock(mutex_);
    if (!quiet_) {
        std::cout << line;
        std::cout.flush();
    }
    if (file_.is_open()) {
        file_ << line;
        file_.flush();
    }
}

std::string ShortFunctionName(std::string_view prettyFunction)
{
    std::string text{prettyFunction};

    // GCC template details: "... [with T = int]".
    if (const auto with = text.find(" [with "); with != std::string::npos)
        text.erase(with);

    // Lambdas report the function that contains them.
    for (const char* lambdaMarker : {"::<lambda", "::(anonymous class)", "::<unnamed"}) {
        if (const auto lambda = text.find(lambdaMarker); lambda != std::string::npos)
            text.erase(lambda);
    }

    // Drop the parameter list and qualifiers after it, matching the last ')'.
    if (const auto close = text.rfind(')'); close != std::string::npos) {
        int depth = 0;
        for (std::size_t i = close + 1; i-- > 0;) {
            if (text[i] == ')') {
                ++depth;
            } else if (text[i] == '(') {
                if (--depth == 0) {
                    text.erase(i);
                    break;
                }
            }
        }
    }

    text = RemoveAll(std::move(text), "{anonymous}::");
    text = RemoveAll(std::move(text), "(anonymous namespace)::");
    text = RemoveTemplateArguments(text);

    // The name is the last space-separated token; earlier tokens are the
    // return type and specifiers ("static", "virtual", "void", ...).
    if (const auto space = text.rfind(' '); space != std::string::npos)
        text.erase(0, space + 1);
    while (!text.empty() && (text.front() == '*' || text.front() == '&'))
        text.erase(0, 1);

    // Keep at most "Owner::function".
    const auto last = text.rfind("::");
    if (last != std::string::npos && last > 0) {
        const auto previous = text.rfind("::", last - 1);
        if (previous != std::string::npos)
            text.erase(0, previous + 2);
    }

    return text.empty() ? std::string{"unknown"} : text;
}

std::string CurrentUtcLogTimestamp()
{
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto milliseconds = duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    const std::tm calendar = UtcCalendar(system_clock::to_time_t(now));

    return fmt::format("{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}.{:03d}",
                       calendar.tm_year + 1900, calendar.tm_mon + 1, calendar.tm_mday,
                       calendar.tm_hour, calendar.tm_min, calendar.tm_sec,
                       static_cast<int>(milliseconds.count()));
}

std::string FormatUsd(double value)
{
    if (!std::isfinite(value))
        return "unavailable USD";

    const bool negative = value < 0;
    const long long cents = std::llround(std::fabs(value) * 100.0);
    std::string whole = std::to_string(cents / 100);
    for (int position = static_cast<int>(whole.size()) - 3; position > 0; position -= 3)
        whole.insert(static_cast<std::size_t>(position), ",");

    return fmt::format("{}{}.{:02d} USD", negative ? "-" : "", whole, static_cast<int>(cents % 100));
}

std::string FormatQuantity(double value)
{
    if (!std::isfinite(value))
        return "unavailable";
    std::string text = fmt::format("{:.10f}", value);
    while (!text.empty() && text.back() == '0')
        text.pop_back();
    if (!text.empty() && text.back() == '.')
        text.pop_back();
    return text == "-0" ? "0" : text;
}

std::string FormatDayKey(int64_t yyyymmdd)
{
    if (yyyymmdd == 0)
        return "none";
    if (yyyymmdd < 10000101 || yyyymmdd > 99991231)
        return std::to_string(yyyymmdd);
    return fmt::format("{:04d}-{:02d}-{:02d}",
                       static_cast<int>(yyyymmdd / 10000),
                       static_cast<int>(yyyymmdd / 100 % 100),
                       static_cast<int>(yyyymmdd % 100));
}

std::string FormatEpochMilliseconds(int64_t epochMilliseconds)
{
    if (epochMilliseconds == 0)
        return "none";
    const std::tm calendar = UtcCalendar(static_cast<std::time_t>(epochMilliseconds / 1000));
    return fmt::format("{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}.{:03d}",
                       calendar.tm_year + 1900, calendar.tm_mon + 1, calendar.tm_mday,
                       calendar.tm_hour, calendar.tm_min, calendar.tm_sec,
                       static_cast<int>(epochMilliseconds % 1000));
}
