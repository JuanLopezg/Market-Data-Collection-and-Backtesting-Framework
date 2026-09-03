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
    std::string rollingPath;
    std::string requestedLogDir;
    std::error_code directoryError;

    if (const char* logDir = std::getenv("ALGOTRADING_LOG_DIR"); logDir && *logDir) {
        requestedLogDir = logDir;
        const std::filesystem::path directory{logDir};
        std::filesystem::create_directories(directory, directoryError);

        if (!directoryError) {
            filePath = (directory / (serviceName + ".log")).string();
            rollingPath = (directory / (serviceName + "_roll.log")).string();
        }
    }

    Logger::Instance().Setup(
        debug,
        quiet,
        filePath,
        rollingPath,
        true
    );

    if (directoryError) {
        LG_WARN(
            "service={} event=log_directory_unavailable requested_dir={} error={} fallback=docker_stdout",
            serviceName,
            requestedLogDir,
            directoryError.message()
        );
    }

    LG_INFO(
        "service={} event=logger_ready debug={} quiet={} file_logging={} log_dir={}",
        serviceName,
        debug,
        quiet,
        !filePath.empty(),
        filePath.empty() ? "<docker-stdout-only>" : std::filesystem::path(filePath).parent_path().string()
    );
}

} // namespace ServiceLogging
