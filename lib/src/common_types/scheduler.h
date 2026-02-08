#pragma once

#include <thread>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <condition_variable>
#include <mutex>

/**************************************************************************************
 * Purpose : A generic scheduling engine that executes `processSecond()` at a fixed
 *           interval, with timeout handling and both blocking and non-blocking start
 *           modes. The scheduler supports graceful stop using both a global shared
 *           stop flag and a local stop request.
 *
 * Template: TContext - User-defined context object passed to the scheduler.
 **************************************************************************************/
template<typename TContext>
class Scheduler {
public:
    // Global stop flag shared across all Scheduler<TContext> instances.
    static std::atomic<bool> globalStop;

    /**************************************************************************************
     * Purpose : Constructs a scheduler with user context, tick interval, timeout for work,
     *           and optional initial delay before the first tick.
     * Args    : ctx            - Shared pointer to a user-defined context.
     *           interval       - Time between ticks.
     *           timeout        - Maximum time allowed for processSecond() execution.
     *           secondsToStart - Optional delay before the scheduler starts ticking.
     * Return  : None
     **************************************************************************************/
    Scheduler(std::shared_ptr<TContext> ctx,
              std::chrono::milliseconds interval,
              std::chrono::milliseconds timeout,
              std::chrono::seconds secondsToStart = std::chrono::seconds(2))
        : ctx(std::move(ctx)),
          interval(interval),
          timeout(timeout),
          secondsToStart(secondsToStart)
    {}

    virtual ~Scheduler() {
        stop();
    }

    /**************************************************************************************
     * Purpose : Starts the scheduler loop in the current thread (blocking mode).
     *           Executes processSecond() every interval and handles timeouts and stops.
     * Args    : None
     * Return  : void
     **************************************************************************************/
    void start() {
        running = true;

        using clock = std::chrono::high_resolution_clock;
        auto nextTick = clock::now();

        if (secondsToStart.count() > 0) {
            std::this_thread::sleep_for(secondsToStart);
            nextTick = clock::now();
        }

        while (running) {
            auto fut = std::async(std::launch::async, [this]() {
                this->processSecond();
            });

            if (fut.wait_for(timeout) == std::future_status::timeout) {
                onTimeout();
            }

            if (globalStop.load() || !running.load()) {
                break;
            }

            nextTick += interval;

            {
                std::unique_lock<std::mutex> lock(cvMutex);
                cv.wait_until(lock, nextTick, [this]() {
                    return globalStop.load() || !running.load();
                });
            }

            if (globalStop.load() || !running.load()) {
                break;
            }
        }
    }

    /**************************************************************************************
     * Purpose : Starts the scheduler loop in a new thread (non-blocking mode).
     * Args    : None
     * Return  : void
     **************************************************************************************/
    void startAsync() {
        running = true;
        worker = std::thread([this]() { start(); });
    }

    /**************************************************************************************
     * Purpose : Requests the scheduler to stop gracefully and joins the worker thread.
     * Args    : None
     * Return  : void
     **************************************************************************************/
    void stop() {
        running = false;
        cv.notify_all();
        if (worker.joinable())
            worker.join();
    }

protected:
    /**************************************************************************************
     * Purpose : User-implemented function executed each tick of the scheduler.
     * Args    : None
     * Return  : void
     **************************************************************************************/
    virtual void processSecond() = 0;

    /**************************************************************************************
     * Purpose : Called when processSecond() exceeds its timeout. Default: no action.
     * Args    : None
     * Return  : void
     **************************************************************************************/
    virtual void onTimeout() {
        // default: do nothing
    }

    // User-provided shared context for scheduler tasks.
    std::shared_ptr<TContext> ctx;

private:
    // Time between scheduler ticks (process second executions).
    std::chrono::milliseconds interval;

    // Maximum execution time allowed for processSecond().
    std::chrono::milliseconds timeout;

    // Initial delay before the scheduler begins ticking.
    std::chrono::seconds secondsToStart;

    // Indicates whether the scheduler instance is running.
    std::atomic<bool> running{false};

    // Thread used for asynchronous scheduler execution.
    std::thread worker;

    // Condition variable used to handle timed waits between ticks.
    std::condition_variable cv;

    // Mutex used with condition variable.
    std::mutex cvMutex;
};

// Static member definition for template.
template<typename TContext>
std::atomic<bool> Scheduler<TContext>::globalStop = false;
