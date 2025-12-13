#include "logger.h"

#include <log4cpp/OstreamAppender.hh>
#include <log4cpp/FileAppender.hh>
#include <log4cpp/RollingFileAppender.hh>
#include <log4cpp/PatternLayout.hh>

#include <iostream>
#include <chrono>
#include <sstream>

// --------------------------------------------------------------
//  Singleton
// --------------------------------------------------------------
Logger& Logger::Instance() {
    static Logger instance;
    return instance;
}

// --------------------------------------------------------------
//  Setup logger configuration
// --------------------------------------------------------------
void Logger::Setup(bool debugEnabled,
                   bool quiet,
                   const std::string& fileAppenderPath,
                   const std::string& rollingAppenderPath,
                   bool includeHeader)
{
    includeHeader_ = includeHeader;

    log4cpp::Category& root = log4cpp::Category::getRoot();
    root.removeAllAppenders();

    // Logging level
    root.setPriority(debugEnabled ? log4cpp::Priority::DEBUG
                                  : log4cpp::Priority::INFO);

    // -------------------------
    // PatternLayout para TODO
    // -------------------------
    auto* layoutConsole = new log4cpp::PatternLayout();
    layoutConsole->setConversionPattern("%d{%Y-%m-%d %H:%M:%S.%l} [%p] %m%n");

    auto* layoutFile = new log4cpp::PatternLayout();
    layoutFile->setConversionPattern("%d{%Y-%m-%d %H:%M:%S.%l} [%p] %m%n");

    auto* layoutRolling = new log4cpp::PatternLayout();
    layoutRolling->setConversionPattern("%d{%Y-%m-%d %H:%M:%S.%l} [%p] %m%n");

    // Console
    if (!quiet) {
        auto* console = new log4cpp::OstreamAppender("console", &std::cout);
        console->setLayout(layoutConsole);
        root.addAppender(console);
    }

    // File appender
    if (!fileAppenderPath.empty()) {
        auto* fa = new log4cpp::FileAppender("file", fileAppenderPath);
        fa->setLayout(layoutFile);
        root.addAppender(fa);
    }

    // Rolling appender
    if (!rollingAppenderPath.empty()) {
        auto* rfa = new log4cpp::RollingFileAppender(
            "rolling",
            rollingAppenderPath,
            5 * 1024 * 1024,
            5
        );
        rfa->setLayout(layoutRolling);
        root.addAppender(rfa);
    }
}

// --------------------------------------------------------------
//  Get root category
// --------------------------------------------------------------
log4cpp::Category& Logger::GetRootCategory() {
    return log4cpp::Category::getRoot();
}

// --------------------------------------------------------------
//  HandleLog implementation
// --------------------------------------------------------------
void HandleLog(LogSeverityLevel severity,
               const std::string& event,
               const char* file,
               const char* function,
               uint32_t line)
{
    log4cpp::Category& root = Logger::Instance().GetRootCategory();

    log4cpp::Priority::Value p = log4cpp::Priority::INFO;
    switch (severity) {
        case LogSeverityLevel::DEBUG: p = log4cpp::Priority::DEBUG; break;
        case LogSeverityLevel::INFO:  p = log4cpp::Priority::INFO;  break;
        case LogSeverityLevel::WARN:  p = log4cpp::Priority::WARN;  break;
        case LogSeverityLevel::ERROR: p = log4cpp::Priority::ERROR; break;
        case LogSeverityLevel::ALERT: p = log4cpp::Priority::ALERT; break;
    }

    std::ostringstream ss;

    if (Logger::Instance().IncludeHeader()) {
        ss << file << ";"
           << function << ";"
           << line << ";";
    }

    ss << event;

    root << p << ss.str();
}
