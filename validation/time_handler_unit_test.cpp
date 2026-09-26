#include "time_handler.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>


namespace {

using namespace std::chrono_literals;
using Clock = TimeHandler::Clock;
using Duration = TimeHandler::Duration;
using TimePoint = TimeHandler::TimePoint;

int failures = 0;

void check(bool condition, const std::string& name)
{
    if (condition) {
        std::cout << "PASS: " << name << '\n';
        return;
    }

    ++failures;
    std::cerr << "FAIL: " << name << '\n';
}

Duration absolute(Duration value)
{
    return value < Duration::zero() ? -value : value;
}

bool near(TimePoint actual, TimePoint expected, Duration tolerance)
{
    return absolute(actual - expected) <= tolerance;
}

TimePoint utc(int year, unsigned month, unsigned day, int hour = 0, int minute = 0, int second = 0)
{
    using namespace std::chrono;
    return sys_days{std::chrono::year{year} / month / day} +
           hours{hour} + minutes{minute} + seconds{second};
}

void testLiveIdentity()
{
    const auto reference = Clock::now();
    TimeHandler handler(1.0, Duration::zero(), reference);

    const auto before = Clock::now();
    const auto business = handler.getTime();
    const auto after = Clock::now();

    check(business >= before && business <= after, "speed=1,bias=0 follows real UTC");
}

void testBiasAtOneX()
{
    const auto reference = Clock::now();
    const auto bias = std::chrono::duration_cast<Duration>(-48h);
    TimeHandler handler(1.0, bias, reference);

    const auto real = Clock::now();
    const auto business = handler.getTime();
    check(near(business, real + bias, 20ms), "speed=1 applies bias exactly");
}

void testDoubleSpeed()
{
    const auto realNow = Clock::now();
    const auto reference = realNow - 2s;
    TimeHandler handler(2.0, Duration::zero(), reference);

    const auto business = handler.getTime();
    const auto expected = reference + 4s;
    check(near(business, expected, 50ms), "speed=2 scales elapsed real time");
}

void testHalfSpeed()
{
    const auto realNow = Clock::now();
    const auto reference = realNow - 2s;
    TimeHandler handler(0.5, Duration::zero(), reference);

    const auto business = handler.getTime();
    const auto expected = reference + 1s;
    check(near(business, expected, 50ms), "speed=0.5 scales elapsed real time");
}


void testFractionalSpeedPrecision()
{
    const auto realNow = Clock::now();
    const auto reference = realNow - 4s;
    TimeHandler handler(1.25, Duration::zero(), reference);

    const auto business = handler.getTime();
    const auto expected = reference + 5s;
    check(near(business, expected, 50ms), "fractional speed preserves sub-second precision");
}

void testSharedReference()
{
    const auto reference = Clock::now() - 5s;
    const auto bias = std::chrono::duration_cast<Duration>(-24h);
    TimeHandler first(100.0, bias, reference);
    TimeHandler second(100.0, bias, reference);

    const auto a = first.getTime();
    const auto b = second.getTime();
    check(absolute(a - b) < 100ms, "same config/reference yields same business timeline");
}

void testPastSimulatedDate()
{
    const auto reference = Clock::now();
    const auto simulatedReference = utc(2020, 1, 1);
    const auto bias = simulatedReference - reference;
    TimeHandler handler(1.0, bias, reference);

    const auto business = handler.getTime();
    check(near(business, simulatedReference, 50ms), "negative multi-year bias maps into historical time");
}

void testLeapYearBoundary()
{
    // Anchor the real reference two seconds in the past and map that instant to
    // 2024-02-28 00:00 UTC. At 43200x, two real seconds correspond to one
    // simulated day, so getTime() must land on leap day (2024-02-29).
    const auto realNow = Clock::now();
    const auto realReference = realNow - 2s;
    const auto simulatedReference = utc(2024, 2, 28);
    const auto bias = simulatedReference - realReference;
    TimeHandler handler(43200.0, bias, realReference);

    const auto businessDay = std::chrono::floor<std::chrono::days>(handler.getTime());
    const std::chrono::year_month_day ymd{businessDay};

    check(
        ymd.year() == std::chrono::year{2024} &&
        ymd.month() == std::chrono::month{2} &&
        ymd.day() == std::chrono::day{29},
        "formula crosses 2024 leap-day boundary correctly"
    );
}

void testInvalidSpeed()
{
    const auto reference = Clock::now();
    const auto expectInvalid = [&](double speed, const std::string& name) {
        bool threw = false;
        try {
            TimeHandler handler(speed, Duration::zero(), reference);
            (void)handler;
        }
        catch (const std::invalid_argument&) {
            threw = true;
        }
        check(threw, name);
    };

    expectInvalid(0.0, "speed=0 rejected");
    expectInvalid(-1.0, "negative speed rejected");
    expectInvalid(std::numeric_limits<double>::infinity(), "infinite speed rejected");
    expectInvalid(std::numeric_limits<double>::quiet_NaN(), "NaN speed rejected");
}

void testSleepScalingFast()
{
    TimeHandler handler(10.0, Duration::zero(), Clock::now());

    const auto started = std::chrono::steady_clock::now();
    handler.sleep(std::chrono::duration_cast<Duration>(200ms));
    const auto elapsed = std::chrono::steady_clock::now() - started;

    check(elapsed >= 15ms, "sleep scales business duration down at 10x (lower bound)");
    check(elapsed < 500ms, "sleep scales business duration down at 10x (upper bound)");
}

void testSleepScalingSlow()
{
    TimeHandler handler(0.5, Duration::zero(), Clock::now());

    const auto started = std::chrono::steady_clock::now();
    handler.sleep(std::chrono::duration_cast<Duration>(50ms));
    const auto elapsed = std::chrono::steady_clock::now() - started;

    check(elapsed >= 80ms, "sleep scales business duration up at 0.5x (lower bound)");
    check(elapsed < 750ms, "sleep scales business duration up at 0.5x (upper bound)");
}

void testSleepNoOp()
{
    TimeHandler handler(1.0, Duration::zero(), Clock::now());

    const auto started = std::chrono::steady_clock::now();
    handler.sleep(Duration::zero());
    handler.sleep(std::chrono::duration_cast<Duration>(-1s));
    handler.sleepUntil(handler.getTime() - 1s);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    check(elapsed < 100ms, "non-positive/past business waits return immediately");
}

void testSleepUntilScaling()
{
    TimeHandler handler(20.0, Duration::zero(), Clock::now());
    const auto target = handler.getTime() + 400ms;

    const auto started = std::chrono::steady_clock::now();
    handler.sleepUntil(target);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto reached = handler.getTime();

    check(elapsed >= 15ms, "sleepUntil scales target wait by speed (lower bound)");
    check(elapsed < 500ms, "sleepUntil scales target wait by speed (upper bound)");
    check(reached + 10ms >= target, "sleepUntil reaches requested business timestamp");
}

} // namespace


int main()
{
    testLiveIdentity();
    testBiasAtOneX();
    testDoubleSpeed();
    testHalfSpeed();
    testFractionalSpeedPrecision();
    testSharedReference();
    testPastSimulatedDate();
    testLeapYearBoundary();
    testInvalidSpeed();
    testSleepScalingFast();
    testSleepScalingSlow();
    testSleepNoOp();
    testSleepUntilScaling();

    if (failures != 0) {
        std::cerr << "\nFAIL: TimeHandler unit tests: " << failures << " failure(s)\n";
        return EXIT_FAILURE;
    }

    std::cout << "\nPASS: TimeHandler unit tests\n";
    return EXIT_SUCCESS;
}
