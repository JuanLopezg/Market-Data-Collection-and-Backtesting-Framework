// Map real time to business time for shared live clocks and replay pacing.
// Historical market timestamps remain supplied by replay events, not generated here.

#include "time_handler.h"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <thread>


namespace {

using Duration = TimeHandler::Duration;
using Rep = Duration::rep;

long double durationTicks(Duration value)
{
    using LongDuration = std::chrono::duration<long double, Duration::period>;
    return LongDuration(value).count();
}

} // namespace


Duration TimeHandler::scaleDuration(Duration duration, long double factor)
{
    if (!std::isfinite(factor))
        throw std::overflow_error("TimeHandler duration scale factor is not finite");

    const long double scaledTicks = durationTicks(duration) * factor;
    if (!std::isfinite(scaledTicks))
        throw std::overflow_error("TimeHandler scaled duration is not finite");

    const long double minTicks = static_cast<long double>(std::numeric_limits<Rep>::lowest());
    const long double maxTicks = static_cast<long double>(std::numeric_limits<Rep>::max());
    if (scaledTicks < minTicks || scaledTicks > maxTicks)
        throw std::overflow_error("TimeHandler scaled duration exceeds clock range");

    return Duration{static_cast<Rep>(scaledTicks)}; // Discard sub-tick fractions after checking the clock range.
}


TimeHandler::TimePoint TimeHandler::businessTimeAt(TimePoint realTime) const
{
    const Duration realElapsed = realTime - reference_time_;
    return reference_time_ + bias_ + scaleDuration(realElapsed, speed_);
}


TimeHandler::Duration TimeHandler::realDurationFor(Duration businessDuration) const
{
    return scaleDuration(businessDuration, 1.0L / static_cast<long double>(speed_)); // Invert speed for real waits.
}


TimeHandler::TimeHandler(double speed, Duration bias, TimePoint referenceTime)
    : speed_(speed),
      bias_(bias),
      reference_time_(referenceTime)
{
    if (!std::isfinite(speed_) || speed_ <= 0.0)
        throw std::invalid_argument("TimeHandler speed must be finite and greater than zero");
}


TimeHandler::TimePoint TimeHandler::getTime() const
{
    return businessTimeAt(Clock::now());
}


void TimeHandler::sleep(Duration businessDuration) const
{
    if (businessDuration <= Duration::zero())
        return;

    const Duration realDuration = realDurationFor(businessDuration);
    if (realDuration <= Duration::zero())
        return;

    std::this_thread::sleep_for(realDuration);
}


void TimeHandler::sleepUntil(TimePoint businessTime) const
{
    const TimePoint now = getTime();
    if (businessTime <= now)
        return;

    sleep(businessTime - now);
}
