#pragma once

#include <memory>
#include <chrono>
#include <ctime>
#include "logger.h"
#include "database_configdata.h"
#include "config_handler.h"
#include "scheduler.h"
#include "database_downloader.h"

// --------------------------------------------------------------------------------------
// Context used by the DatabaseScheduler. Holds the current database configuration.
// --------------------------------------------------------------------------------------
struct DatabaseContext {
    DatabaseConfig config;   // Current active database configuration
};

using DatabaseConfigHandler = ConfigHandler<DatabaseConfig>;

/**************************************************************************************
 * Purpose : Scheduler responsible for periodically executing database update work.
 *           It monitors configuration changes through a ConfigHandler and also handles
 *           daily tasks such as midnight-triggered operations (e.g., full downloads).
 *
 * Inheritance:
 *    - Inherits from Scheduler<DatabaseContext> to gain periodic scheduling capabilities.
 **************************************************************************************/
class DatabaseScheduler : public Scheduler<DatabaseContext> {
public:

    /**************************************************************************************
     * Purpose : Constructs a database scheduler responsible for running periodic work
     *           and reacting to updated configuration values.
     * Args    : ctx            - Shared pointer containing database context.
     *           configHandler  - Config handler that provides updated configurations.
     *           interval       - Time between scheduler ticks.
     *           timeout        - Max time allowed for each processSecond() execution.
     *           secondsToStart - Optional initial delay before scheduling begins.
     * Return  : None
     **************************************************************************************/
    DatabaseScheduler(std::shared_ptr<DatabaseContext> ctx,
                      DatabaseConfigHandler& configHandler,
                      std::chrono::milliseconds interval,
                      std::chrono::milliseconds timeout,
                      std::chrono::seconds secondsToStart = std::chrono::seconds(1));

protected:

    /**************************************************************************************
     * Purpose : Main periodic task executed by the scheduler. Performs database update
     *           operations, applies new configuration when available, and triggers 
     *           special actions (e.g., daily midnight downloads).
     * Args    : None
     * Return  : void
     **************************************************************************************/
    void processSecond() override;

private:
    // Reference to configuration handler used to detect and consume new configs.
    DatabaseConfigHandler& configHandler_;

    // Worker object responsible for contacting the remote database endpoints.
    DatabaseDownloader databaseDownloader_;

    // Timestamp of the next scheduled UTC midnight event.
    std::chrono::system_clock::time_point nextMidnightUTC_;

    // boolean for first iteration of scheduler
    bool firtsIteration = true;
};
