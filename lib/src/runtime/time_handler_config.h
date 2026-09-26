#pragma once

#include <chrono>

#include "time_handler.h"


/**************************************************************************************
 * Type    : TimeHandlerConfig
 * Purpose : Canonical, process-local representation of business-time configuration.
 *
 * Operators provide:
 *   - speed
 *   - one REAL UTC reference
 *   - the SIMULATED UTC time corresponding to that same instant
 *
 * The bias consumed by TimeHandler is derived centrally as:
 *
 *     bias = simulated_reference_utc - real_reference_utc
 *
 * This avoids hand-calculating offsets across leap years/calendar boundaries and keeps
 * every service on exactly the same configuration vocabulary.
 **************************************************************************************/
struct TimeHandlerConfig final {
    using TimePoint = TimeHandler::TimePoint;
    using Duration = TimeHandler::Duration;

    double speed = 1.0;
    TimePoint real_reference_utc{};
    TimePoint simulated_reference_utc{};

    Duration bias() const noexcept
    {
        return simulated_reference_utc - real_reference_utc;
    }

    bool identity() const noexcept
    {
        return speed == 1.0 && bias() == Duration::zero();
    }
};
