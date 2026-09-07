#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <stdexcept>

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
 *
 * LIVE/TESTNET stay intentionally simple: no NATS clock messages, no simulation state,
 * no speed controls and no extra infrastructure dependency are required to read time.
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

/**************************************************************************************
 * Purpose : Mutable logical clock used by REPLAY consumers.
 *
 * The clock only accepts monotonic revisions and non-decreasing logical time. A newer
 * revision may keep the same time (needed later for pause/speed changes), but neither a
 * stale revision nor a backwards timestamp can silently replace current state.
 **************************************************************************************/
class SimulatedClock final : public Clock {
public:
    using time_point = std::chrono::system_clock::time_point;

    void synchronize(time_point logicalTime, std::uint64_t revision)
    {
        if (revision == 0)
            throw std::invalid_argument("SimulatedClock revision must be positive");

        std::scoped_lock lock(mutex_);

        if (!initialized_) {
            logical_time_ = logicalTime;
            revision_ = revision;
            initialized_ = true;
            return;
        }

        if (revision < revision_)
            throw std::invalid_argument("SimulatedClock revision cannot move backwards");

        if (revision == revision_) {
            if (logicalTime != logical_time_)
                throw std::invalid_argument("SimulatedClock same revision has conflicting time");
            return; // idempotent redelivery
        }

        if (logicalTime < logical_time_)
            throw std::invalid_argument("SimulatedClock logical time cannot move backwards");

        logical_time_ = logicalTime;
        revision_ = revision;
    }

    time_point now() const override
    {
        std::scoped_lock lock(mutex_);
        if (!initialized_)
            throw std::logic_error("SimulatedClock has not received logical time yet");
        return logical_time_;
    }

    bool initialized() const
    {
        std::scoped_lock lock(mutex_);
        return initialized_;
    }

    std::uint64_t revision() const
    {
        std::scoped_lock lock(mutex_);
        return revision_;
    }

private:
    mutable std::mutex mutex_;
    time_point logical_time_{};
    std::uint64_t revision_ = 0;
    bool initialized_ = false;
};
