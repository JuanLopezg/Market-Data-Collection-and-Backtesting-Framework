#pragma once

#include <chrono>

/**************************************************************************************
 * Purpose : Abstract time source used by runtime components whose trading semantics
 *           must not depend directly on the host machine clock.
 **************************************************************************************/
class Clock {
public:
    virtual ~Clock() = default;

    virtual std::chrono::system_clock::time_point now() const = 0;
};

/**************************************************************************************
 * Purpose : Production clock backed by the host system clock.
 **************************************************************************************/
class SystemClock final : public Clock {
public:
    std::chrono::system_clock::time_point now() const override
    {
        return std::chrono::system_clock::now();
    }
};

/**************************************************************************************
 * Purpose : Fixed UTC clock used by deterministic historical simulation/tests.
 **************************************************************************************/
class FixedClock final : public Clock {
public:
    explicit FixedClock(std::chrono::system_clock::time_point fixedTime)
        : fixedTime_(fixedTime)
    {
    }

    std::chrono::system_clock::time_point now() const override
    {
        return fixedTime_;
    }

private:
    std::chrono::system_clock::time_point fixedTime_;
};
