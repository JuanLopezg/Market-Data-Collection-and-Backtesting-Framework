#include "time_handler_factory.h"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>


namespace {

using TimePoint = TimeHandlerConfig::TimePoint;

std::optional<std::string> envValue(const char* name)
{
    const char* value = std::getenv(name);
    if (!value || *value == '\0')
        return std::nullopt;
    return std::string{value};
}

int parseFixedInt(std::string_view text, std::size_t offset, std::size_t count, const char* field)
{
    if (offset + count > text.size())
        throw std::invalid_argument(std::string{"Invalid UTC timestamp: missing "} + field);

    int value = 0;
    const char* begin = text.data() + offset;
    const char* end = begin + count;
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end)
        throw std::invalid_argument(std::string{"Invalid UTC timestamp: invalid "} + field);
    return value;
}

TimePoint parseUtcTimestamp(std::string_view text)
{
    // Strict form: YYYY-MM-DDTHH:MM:SSZ
    if (text.size() != 20 ||
        text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':' || text[19] != 'Z') {
        throw std::invalid_argument(
            "UTC timestamp must use strict YYYY-MM-DDTHH:MM:SSZ format"
        );
    }

    const int year = parseFixedInt(text, 0, 4, "year");
    const int month = parseFixedInt(text, 5, 2, "month");
    const int day = parseFixedInt(text, 8, 2, "day");
    const int hour = parseFixedInt(text, 11, 2, "hour");
    const int minute = parseFixedInt(text, 14, 2, "minute");
    const int second = parseFixedInt(text, 17, 2, "second");

    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59)
        throw std::invalid_argument("UTC timestamp contains an invalid time-of-day");

    const std::chrono::year_month_day ymd{
        std::chrono::year{year},
        std::chrono::month{static_cast<unsigned>(month)},
        std::chrono::day{static_cast<unsigned>(day)}
    };
    if (!ymd.ok())
        throw std::invalid_argument("UTC timestamp contains an invalid calendar date");

    return std::chrono::sys_days{ymd}
        + std::chrono::hours{hour}
        + std::chrono::minutes{minute}
        + std::chrono::seconds{second};
}

double parseSpeed(std::string_view text)
{
    std::string owned{text};
    std::size_t consumed = 0;
    double value = 0.0;

    try {
        value = std::stod(owned, &consumed);
    }
    catch (const std::exception&) {
        throw std::invalid_argument("ALGOTRADING_TIME_SPEED must be a finite number greater than zero");
    }

    if (consumed != owned.size() || !std::isfinite(value) || value <= 0.0)
        throw std::invalid_argument("ALGOTRADING_TIME_SPEED must be a finite number greater than zero");

    return value;
}

void validateConfig(const TimeHandlerConfig& config)
{
    if (!std::isfinite(config.speed) || config.speed <= 0.0)
        throw std::invalid_argument("TimeHandlerConfig speed must be finite and greater than zero");
}

} // namespace


namespace TimeHandlerFactory {

TimeHandlerConfig loadConfigFromEnvironment()
{
    TimeHandlerConfig config;

    const auto speedText = envValue(SPEED_ENV);
    const auto realReferenceText = envValue(REAL_REFERENCE_ENV);
    const auto simulatedReferenceText = envValue(SIMULATED_REFERENCE_ENV);

    config.speed = speedText ? parseSpeed(*speedText) : 1.0;

    const bool hasRealReference = realReferenceText.has_value();
    const bool hasSimulatedReference = simulatedReferenceText.has_value();

    if (hasRealReference != hasSimulatedReference) {
        throw std::invalid_argument(
            "ALGOTRADING_TIME_REAL_REFERENCE_UTC and "
            "ALGOTRADING_TIME_SIMULATED_REFERENCE_UTC must be provided together"
        );
    }

    if (!hasRealReference) {
        if (config.speed != 1.0) {
            throw std::invalid_argument(
                "Non-1.0 ALGOTRADING_TIME_SPEED requires both shared UTC references"
            );
        }

        // Identity LIVE configuration. Reference is algebraically irrelevant when
        // speed=1 and bias=0, so epoch is deterministic and avoids calling now()
        // independently in each service.
        config.real_reference_utc = TimePoint{};
        config.simulated_reference_utc = TimePoint{};
        return config;
    }

    config.real_reference_utc = parseUtcTimestamp(*realReferenceText);
    config.simulated_reference_utc = parseUtcTimestamp(*simulatedReferenceText);

    validateConfig(config);
    return config;
}


TimeHandler create(const TimeHandlerConfig& config)
{
    validateConfig(config);
    return TimeHandler{config.speed, config.bias(), config.real_reference_utc};
}


TimeHandler createFromEnvironment()
{
    return create(loadConfigFromEnvironment());
}

} // namespace TimeHandlerFactory
