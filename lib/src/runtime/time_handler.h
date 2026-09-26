#pragma once

#include <chrono>


/**************************************************************************************
 * Type    : TimeHandler
 * Purpose : Provide local business/event time without a distributed clock authority
 *
 * Formula:
 *
 *     T_business = T_reference
 *                + bias
 *                + speed * (T_real - T_reference)
 *
 * The handler is intentionally local and immutable for the lifetime of a process.
 * It owns no transport, persistence, synchronization protocol, pause/resume state, or
 * distributed authority.
 *
 * IMPORTANT:
 *   - getTime() is for BUSINESS / EVENT time.
 *   - sleep()/sleepUntil() are only for BUSINESS-duration waits.
 *   - network/database/NATS timeouts, retry backoff, health checks, polling cadence,
 *     latency measurement and watchdogs must continue to use real/monotonic time.
 **************************************************************************************/
class TimeHandler final {
public:
    using Clock = std::chrono::system_clock;
    using TimePoint = Clock::time_point;
    using Duration = Clock::duration;

private:
    double speed_;
    Duration bias_;
    TimePoint reference_time_;

    static Duration scaleDuration(Duration duration, long double factor);
    TimePoint businessTimeAt(TimePoint realTime) const;
    Duration realDurationFor(Duration businessDuration) const;

public:
    TimeHandler(double speed, Duration bias, TimePoint referenceTime);

    TimePoint getTime() const;

    // Sleep for a BUSINESS/EVENT duration. The real wait is businessDuration / speed.
    // Non-positive durations are already satisfied and therefore return immediately.
    void sleep(Duration businessDuration) const;

    // Sleep until a BUSINESS/EVENT timestamp according to this handler's timeline.
    // Targets at or before getTime() return immediately.
    void sleepUntil(TimePoint businessTime) const;
};
