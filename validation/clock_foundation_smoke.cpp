#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>

#include "clock.h"

namespace {

using ClockPoint = std::chrono::system_clock::time_point;

ClockPoint point(std::int64_t seconds)
{
    return ClockPoint{std::chrono::seconds{seconds}};
}

template <typename Fn>
void requireThrows(Fn&& fn, const char* message)
{
    bool threw = false;
    try {
        fn();
    }
    catch (const std::exception&) {
        threw = true;
    }
    if (!threw)
        throw std::runtime_error(message);
}

} // namespace

int main()
{
    SimulatedClock clock;

    requireThrows([&] { (void)clock.now(); }, "uninitialized now() must fail");
    requireThrows([&] { clock.synchronize(point(10), 0); }, "revision zero must fail");

    clock.synchronize(point(10), 1);
    if (!clock.initialized() || clock.revision() != 1 || clock.now() != point(10))
        throw std::runtime_error("initial synchronization failed");

    // Same revision + same time is an idempotent redelivery.
    clock.synchronize(point(10), 1);

    requireThrows(
        [&] { clock.synchronize(point(11), 1); },
        "same revision with conflicting logical time must fail"
    );
    requireThrows(
        [&] { clock.synchronize(point(9), 2); },
        "newer revision must not move logical time backwards"
    );
    requireThrows(
        [&] { clock.synchronize(point(10), 0); },
        "stale/zero revision must fail"
    );

    // A newer revision may keep the same logical time for future pause/speed changes.
    clock.synchronize(point(10), 2);
    if (clock.revision() != 2 || clock.now() != point(10))
        throw std::runtime_error("same-time newer revision failed");

    clock.synchronize(point(12), 3);
    if (clock.revision() != 3 || clock.now() != point(12))
        throw std::runtime_error("forward logical-time synchronization failed");

    std::cout << "SIMULATED_CLOCK_MONOTONIC_SMOKE: PASS\n";
    return 0;
}
