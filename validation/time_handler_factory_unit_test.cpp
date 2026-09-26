#include "time_handler_factory.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>


namespace {

using namespace std::chrono_literals;

struct ScopedEnv final {
    std::string name;
    std::optional<std::string> old;

    explicit ScopedEnv(const char* key) : name(key)
    {
        if (const char* value = std::getenv(key))
            old = std::string{value};
    }

    ~ScopedEnv()
    {
        if (old)
            ::setenv(name.c_str(), old->c_str(), 1);
        else
            ::unsetenv(name.c_str());
    }
};

struct TimeEnvGuard final {
    ScopedEnv speed{TimeHandlerFactory::SPEED_ENV};
    ScopedEnv realRef{TimeHandlerFactory::REAL_REFERENCE_ENV};
    ScopedEnv simRef{TimeHandlerFactory::SIMULATED_REFERENCE_ENV};

    TimeEnvGuard()
    {
        ::unsetenv(TimeHandlerFactory::SPEED_ENV);
        ::unsetenv(TimeHandlerFactory::REAL_REFERENCE_ENV);
        ::unsetenv(TimeHandlerFactory::SIMULATED_REFERENCE_ENV);
    }
};

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

template <typename F>
void requireInvalid(F&& fn, const std::string& label)
{
    try {
        fn();
    }
    catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error(label + " did not throw std::invalid_argument");
}

void set(const char* name, const char* value)
{
    if (::setenv(name, value, 1) != 0)
        throw std::runtime_error(std::string{"setenv failed for "} + name);
}

void testDefaultLiveIdentity()
{
    TimeEnvGuard guard;
    const auto config = TimeHandlerFactory::loadConfigFromEnvironment();

    require(config.speed == 1.0, "default LIVE speed must be 1.0");
    require(config.bias() == TimeHandler::Duration::zero(), "default LIVE bias must be zero");
    require(config.identity(), "default LIVE config must be identity");
    require(config.real_reference_utc == TimeHandler::TimePoint{}, "default LIVE real ref must be deterministic epoch");
    require(config.simulated_reference_utc == TimeHandler::TimePoint{}, "default LIVE simulated ref must be deterministic epoch");

    const auto before = TimeHandler::Clock::now();
    const auto handler = TimeHandlerFactory::create(config);
    const auto business = handler.getTime();
    const auto after = TimeHandler::Clock::now();

    require(business >= before - 2ms && business <= after + 2ms,
            "default LIVE handler must track real UTC");
}

void testSharedReferencesAndDerivedBias()
{
    TimeEnvGuard guard;
    set(TimeHandlerFactory::SPEED_ENV, "100");
    set(TimeHandlerFactory::REAL_REFERENCE_ENV, "2026-09-20T13:00:00Z");
    set(TimeHandlerFactory::SIMULATED_REFERENCE_ENV, "2020-01-01T00:00:00Z");

    const auto config = TimeHandlerFactory::loadConfigFromEnvironment();
    require(config.speed == 100.0, "speed must parse exactly");
    require(!config.identity(), "historical replay config cannot be identity");

    const auto expectedReal = std::chrono::sys_days{std::chrono::year{2026}/9/20} + 13h;
    const auto expectedSim = std::chrono::sys_days{std::chrono::year{2020}/1/1};
    require(config.real_reference_utc == expectedReal, "real reference mismatch");
    require(config.simulated_reference_utc == expectedSim, "simulated reference mismatch");
    require(config.bias() == expectedSim - expectedReal, "bias must be derived from references");

    const auto handler = TimeHandlerFactory::create(config);
    (void)handler;
}

void testFractionalSpeed()
{
    TimeEnvGuard guard;
    set(TimeHandlerFactory::SPEED_ENV, "0.5");
    set(TimeHandlerFactory::REAL_REFERENCE_ENV, "2024-02-29T12:34:56Z");
    set(TimeHandlerFactory::SIMULATED_REFERENCE_ENV, "2024-02-29T12:34:56Z");

    const auto config = TimeHandlerFactory::loadConfigFromEnvironment();
    require(std::abs(config.speed - 0.5) < 1e-12, "fractional speed mismatch");
    require(config.bias() == TimeHandler::Duration::zero(), "equal references must yield zero bias");
}

void testReferencePairIsAtomic()
{
    {
        TimeEnvGuard guard;
        set(TimeHandlerFactory::REAL_REFERENCE_ENV, "2026-09-20T13:00:00Z");
        requireInvalid([] { (void)TimeHandlerFactory::loadConfigFromEnvironment(); },
                       "real reference without simulated reference");
    }
    {
        TimeEnvGuard guard;
        set(TimeHandlerFactory::SIMULATED_REFERENCE_ENV, "2020-01-01T00:00:00Z");
        requireInvalid([] { (void)TimeHandlerFactory::loadConfigFromEnvironment(); },
                       "simulated reference without real reference");
    }
}

void testNonIdentitySpeedRequiresReferences()
{
    TimeEnvGuard guard;
    set(TimeHandlerFactory::SPEED_ENV, "2");
    requireInvalid([] { (void)TimeHandlerFactory::loadConfigFromEnvironment(); },
                   "speed != 1 without references");
}

void testInvalidSpeeds()
{
    for (const char* bad : {"0", "-1", "nan", "inf", "abc", "2x", ""}) {
        TimeEnvGuard guard;
        set(TimeHandlerFactory::SPEED_ENV, bad);
        if (std::string{bad}.empty()) {
            // Empty is treated as unset, therefore LIVE identity is valid.
            const auto config = TimeHandlerFactory::loadConfigFromEnvironment();
            require(config.identity(), "empty speed env must behave as unset");
        }
        else {
            requireInvalid([] { (void)TimeHandlerFactory::loadConfigFromEnvironment(); },
                           std::string{"invalid speed: "} + bad);
        }
    }
}

void testStrictUtcFormatAndCalendarValidation()
{
    const char* invalid[] = {
        "2026-09-20 13:00:00Z",
        "2026-09-20T13:00:00+00:00",
        "2026-09-20T13:00Z",
        "2026-02-29T13:00:00Z",
        "2024-13-01T00:00:00Z",
        "2024-01-01T24:00:00Z",
        "2024-01-01T00:60:00Z",
        "2024-01-01T00:00:60Z"
    };

    for (const char* bad : invalid) {
        TimeEnvGuard guard;
        set(TimeHandlerFactory::REAL_REFERENCE_ENV, bad);
        set(TimeHandlerFactory::SIMULATED_REFERENCE_ENV, "2020-01-01T00:00:00Z");
        requireInvalid([] { (void)TimeHandlerFactory::loadConfigFromEnvironment(); },
                       std::string{"invalid timestamp: "} + bad);
    }
}

void testLeapDayAccepted()
{
    TimeEnvGuard guard;
    set(TimeHandlerFactory::SPEED_ENV, "10");
    set(TimeHandlerFactory::REAL_REFERENCE_ENV, "2024-02-29T23:59:59Z");
    set(TimeHandlerFactory::SIMULATED_REFERENCE_ENV, "2020-02-29T23:59:59Z");
    const auto config = TimeHandlerFactory::loadConfigFromEnvironment();
    require(config.speed == 10.0, "leap-day config speed mismatch");
}

} // namespace


int main()
{
    try {
        testDefaultLiveIdentity();
        testSharedReferencesAndDerivedBias();
        testFractionalSpeed();
        testReferencePairIsAtomic();
        testNonIdentitySpeedRequiresReferences();
        testInvalidSpeeds();
        testStrictUtcFormatAndCalendarValidation();
        testLeapDayAccepted();

        std::cout << "PASS: TimeHandler config/factory unit tests\n";
        return 0;
    }
    catch (const std::exception& ex) {
        std::cerr << "FAIL: " << ex.what() << '\n';
        return 1;
    }
}
