// Configure service logging from environment flags.
//   ALGOTRADING_LOG_DEBUG=1  also write DEBUG lines (exact IDs, hashes, raw values)
//   ALGOTRADING_LOG_QUIET=1  do not write to stdout
//   ALGOTRADING_LOG_DIR=dir  also append every line to dir/<service>.log
// Falls back to stdout only when the requested directory cannot be created.

#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>

#include "logger.h"

namespace ServiceLogging {

inline bool envFlag(const char* name)
{
    const char* value = std::getenv(name);
    if (!value)
        return false;

    const std::string text{value};
    return text == "1" || text == "true" || text == "TRUE" ||
           text == "yes" || text == "YES" || text == "on" || text == "ON";
}

inline void setup(const std::string& serviceName)
{
    const bool debug = envFlag("ALGOTRADING_LOG_DEBUG");
    const bool quiet = envFlag("ALGOTRADING_LOG_QUIET");

    std::string filePath;
    std::string requestedLogDir;
    std::error_code directoryError;

    if (const char* logDir = std::getenv("ALGOTRADING_LOG_DIR"); logDir && *logDir) {
        requestedLogDir = logDir;
        const std::filesystem::path directory{logDir};
        std::filesystem::create_directories(directory, directoryError);

        if (!directoryError)
            filePath = (directory / (serviceName + ".log")).string();
    }

    Logger::Instance().Setup(debug, quiet, filePath);

    if (directoryError) {
        LG_WARN("Cannot create log directory {} ({}); writing to stdout only",
                requestedLogDir, directoryError.message());
    }

    LG_INFO("{} logger ready (debug {}, output {})",
            serviceName,
            debug ? "on" : "off",
            filePath.empty() ? std::string{"stdout"} : "stdout + " + filePath);
}

} // namespace ServiceLogging
