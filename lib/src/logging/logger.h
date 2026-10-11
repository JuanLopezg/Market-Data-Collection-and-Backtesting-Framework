// Shared human-readable logger for lib/, live_trading/ and research/.
//
// Every line has the same shape, always in UTC:
//
//   2026-10-07 20:33:50.456 [INFO ] ExecutionStateServiceRuntime::run : Service ready
//
// The function label is derived from the compiler's function signature, so call
// sites only write the message. INFO/WARN/ERROR/ALERT messages are short English
// sentences for a human reader; exact identifiers, hashes and unrounded values
// belong in DEBUG lines. Never log secrets.

#ifndef LOGGER_H
#define LOGGER_H

#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

// format.h (not only core.h): fmt 8 rejects later format.h includes after
// core.h has already instantiated the std::string formatter.
#include <fmt/format.h>

enum class LogSeverityLevel {
    DEBUG,
    INFO,
    WARN,
    ERROR,
    ALERT
};

class Logger {
public:
    static Logger& Instance();

    // debugEnabled: also write DEBUG lines.
    // quiet: do not write to stdout.
    // filePath: optional append-only copy of every line (empty = stdout only).
    void Setup(bool debugEnabled, bool quiet, const std::string& filePath = "");

    bool IsEnabled(LogSeverityLevel severity) const;

    void Write(LogSeverityLevel severity,
               std::string_view functionName,
               std::string_view message);

private:
    Logger() = default;

    mutable std::mutex mutex_;
    bool debugEnabled_ = false;
    bool quiet_ = false;
    std::ofstream file_;
};

// Reduces a compiler signature such as
//   "void {anonymous}::ExecutionStateServiceRuntime::persist(const string&)"
// to "ExecutionStateServiceRuntime::persist". Lambdas report their enclosing
// function. Free functions keep their namespace, e.g. "ServiceLogging::setup".
std::string ShortFunctionName(std::string_view prettyFunction);

// "2026-10-07 20:33:50.456" for the current UTC time.
std::string CurrentUtcLogTimestamp();

// Readable values for log messages.
// FormatUsd(100000) -> "100,000.00 USD"; negative values keep their sign.
std::string FormatUsd(double value);
// FormatQuantity(0.000123456) -> "0.000123456" (no trailing zeros).
std::string FormatQuantity(double value);
// Daily market key YYYYMMDD (the Timestamp type) -> "2026-10-07". Zero -> "none".
std::string FormatDayKey(int64_t yyyymmdd);
// Epoch milliseconds -> "2026-10-07 20:33:50.471" (UTC). Zero -> "none".
std::string FormatEpochMilliseconds(int64_t epochMilliseconds);
// "yes"/"no".
inline const char* YesNo(bool value) { return value ? "yes" : "no"; }

template<typename... Args>
inline std::string FormatLog(fmt::format_string<Args...> format, Args&&... args) {
    return fmt::format(format, std::forward<Args>(args)...);
}

inline std::string FormatLog(const std::string& message) {
    return message;
}

// The static label is computed once per call site; the message is only formatted
// when its severity is enabled (DEBUG is usually disabled outside diagnostics).
#define LG_LOG_AT(severity, ...)                                                       \
    do {                                                                               \
        if (Logger::Instance().IsEnabled(severity)) {                                  \
            static const std::string lgFunctionName = ShortFunctionName(__PRETTY_FUNCTION__); \
            Logger::Instance().Write(severity, lgFunctionName, FormatLog(__VA_ARGS__)); \
        }                                                                              \
    } while (false)

#define LG_DEBUG(...) LG_LOG_AT(LogSeverityLevel::DEBUG, __VA_ARGS__)
#define LG_INFO(...)  LG_LOG_AT(LogSeverityLevel::INFO,  __VA_ARGS__)
#define LG_WARN(...)  LG_LOG_AT(LogSeverityLevel::WARN,  __VA_ARGS__)
#define LG_ERROR(...) LG_LOG_AT(LogSeverityLevel::ERROR, __VA_ARGS__)
#define LG_ALERT(...) LG_LOG_AT(LogSeverityLevel::ALERT, __VA_ARGS__)

#endif
