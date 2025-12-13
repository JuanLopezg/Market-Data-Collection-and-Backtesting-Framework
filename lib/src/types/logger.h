#ifndef LOGGER_H
#define LOGGER_H

#include <log4cpp/Category.hh>
#include <string>
#include <sstream>
#include <cstdint>
#include <fmt/core.h>

// --------------------------------------------------------------
// ENUM severity
// --------------------------------------------------------------
enum class LogSeverityLevel {
    DEBUG,
    INFO,
    WARN,
    ERROR,
    ALERT
};

// --------------------------------------------------------------
// Extract basename of __FILE__
// --------------------------------------------------------------
inline const char* BaseName(const char* path) {
    const char* p = path;
    const char* last = path;
    while (*p) {
        if (*p == '/' || *p == '\\') last = p + 1;
        ++p;
    }
    return last;
}

// --------------------------------------------------------------
// Logger singleton
// --------------------------------------------------------------
class Logger {
public:
    static Logger& Instance();

    void Setup(bool debugEnabled,
               bool quiet,
               const std::string& fileAppenderPath,
               const std::string& rollingAppenderPath,
               bool includeHeader);

    log4cpp::Category& GetRootCategory();

    bool IncludeHeader() const { return includeHeader_; }

    // spdlog-style wrappers
    template<typename... Args>
    static void info(const char* fmtstr, Args&&... args) {
        LG_INFO(fmtstr, std::forward<Args>(args)...);
    }

    template<typename... Args>
    static void warn(const char* fmtstr, Args&&... args) {
        LG_WARN(fmtstr, std::forward<Args>(args)...);
    }

    template<typename... Args>
    static void error(const char* fmtstr, Args&&... args) {
        LG_ERROR(fmtstr, std::forward<Args>(args)...);
    }

    template<typename... Args>
    static void debug(const char* fmtstr, Args&&... args) {
        LG_DEBUG(fmtstr, std::forward<Args>(args)...);
    }

private:
    Logger() = default;
    bool includeHeader_ = true;
};

// --------------------------------------------------------------
// Internal log handler
// --------------------------------------------------------------
void HandleLog(LogSeverityLevel severity,
               const std::string& msg,
               const char* file,
               const char* function,
               uint32_t line);

// --------------------------------------------------------------
// FormatLog — FIXED VERSION USING vformat()
// --------------------------------------------------------------
inline std::string FormatLog(const std::string& s) {
    return s;
}

inline std::string FormatLog(const char* s) {
    return std::string{s};
}

template<typename T>
inline std::string FormatLog(const T& v) {
    return fmt::format("{}", v);
}

template<typename... Args>
inline std::string FormatLog(const char* fmtstr, Args&&... args) {
    return fmt::vformat(fmtstr, fmt::make_format_args(args...));
}

// --------------------------------------------------------------
// Logging macros – same API as spdlog
// --------------------------------------------------------------
#define LG_DEBUG(fmtstr, ...) \
    HandleLog(LogSeverityLevel::DEBUG, FormatLog(fmtstr, ##__VA_ARGS__), BaseName(__FILE__), __func__, __LINE__)

#define LG_INFO(fmtstr, ...) \
    HandleLog(LogSeverityLevel::INFO,  FormatLog(fmtstr, ##__VA_ARGS__), BaseName(__FILE__), __func__, __LINE__)

#define LG_WARN(fmtstr, ...) \
    HandleLog(LogSeverityLevel::WARN,  FormatLog(fmtstr, ##__VA_ARGS__), BaseName(__FILE__), __func__, __LINE__)

#define LG_ERROR(fmtstr, ...) \
    HandleLog(LogSeverityLevel::ERROR, FormatLog(fmtstr, ##__VA_ARGS__), BaseName(__FILE__), __func__, __LINE__)

#define LG_ALERT(fmtstr, ...) \
    HandleLog(LogSeverityLevel::ALERT, FormatLog(fmtstr, ##__VA_ARGS__), BaseName(__FILE__), __func__, __LINE__)

#endif
