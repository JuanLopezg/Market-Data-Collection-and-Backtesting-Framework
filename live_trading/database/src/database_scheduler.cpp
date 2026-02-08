#include "database_scheduler.h"
#include "logger.h"
#include "json_utils.h"
#include "database_downloader.h"
#include "time_utils.h"

/**************************************************************************************
 * Purpose : Constructs the DatabaseScheduler and initializes the underlying Scheduler
 *           timer parameters, configuration handler reference, and database downloader.
 *           Also computes the first UTC midnight trigger for daily tasks.
 * Args    : ctx            - Shared pointer holding the database context.
 *           configHandler  - Configuration handler for detecting and applying updates.
 *           interval       - Time between scheduler ticks.
 *           timeout        - Allowed execution time for processSecond().
 *           secondsToStart - Optional delay before the first scheduler tick.
 * Return  : None
 **************************************************************************************/
DatabaseScheduler::DatabaseScheduler(std::shared_ptr<DatabaseContext> ctx,
                                     DatabaseConfigHandler& configHandler,
                                     std::chrono::milliseconds interval,
                                     std::chrono::milliseconds timeout,
                                     std::chrono::seconds secondsToStart)
    : Scheduler<DatabaseContext>(ctx, interval, timeout, secondsToStart),
      configHandler_(configHandler),
      databaseDownloader_(this->ctx->config.GetDatabasePath())
{
    // Compute when the next UTC midnight event should fire
    nextMidnightUTC_ = computeNextMidnightUTC();
}


/**************************************************************************************
 * Purpose : Core periodic execution function. This runs once every scheduler tick.
 *           Responsible for:
 *             - Logging heartbeat/timing info
 *             - Running the once-per-day midnight update event
 *             - Applying new database configuration when available
 * Args    : None
 * Return  : void
 **************************************************************************************/
void DatabaseScheduler::processSecond() {

    LG_INFO("Running processSecond");
    LG_INFO("Current time: {}, time until UTC midnight: {}",
                 currentUtcTimestamp(), timeUntilUtcMidnight());

    auto& ctxRef = *this->ctx;

    // ============================================================================
    // DAILY UTC MIDNIGHT EVENT — RUNS ONCE PER DAY
    // ============================================================================
    auto now = std::chrono::system_clock::now();

    if (now >= nextMidnightUTC_ || firtsIteration) {
        LG_INFO("Midnight event triggered");
        
        firtsIteration = false;

        bool downloaded =
            databaseDownloader_.downloadData(getPreviousDayDate(getCurrentUtcDate()));
            
        // Schedule the next midnight trigger
        nextMidnightUTC_ = computeNextMidnightUTC();
    }

    // ============================================================================
    // APPLY CONFIGURATION UPDATE IF ONE IS AVAILABLE
    // ============================================================================
    DatabaseConfig newConfig;
    if (configHandler_.consumeNextConfig(newConfig)) {
        LG_INFO("Applying new config...");
        ctxRef.config = std::move(newConfig);
        LG_INFO("Config applied:\n{}",
                     ctxRef.config.ToJson().dump(4));
    }

    LG_INFO("End of process second");
}
