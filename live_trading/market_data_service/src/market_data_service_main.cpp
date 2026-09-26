#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include <curl/curl.h>

#include "market_data_config.h"
#include "market_data_ingestor.h"
#include "market_data_update_publisher.h"
#include "service_logging.h"
#include "time_handler_factory.h"
#include "time_utils.h"
#include "transport_subjects.h"

namespace {

std::atomic<bool> running{true};

void stopHandler(int)
{
    running.store(false);
}

struct Options {
    std::filesystem::path config = "config/market_data/market_data_config.json";
    bool run_once = false;
};

Options parseOptions(int argc, char** argv)
{
    Options result;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto requireValue = [&](const char* option) -> std::string {
            if (i + 1 >= argc)
                throw std::invalid_argument(std::string(option) + " requires a value");
            return argv[++i];
        };

        if (arg == "--config")
            result.config = requireValue("--config");
        else if (arg == "--run-once")
            result.run_once = true;
        else if (arg == "--help" || arg == "-h") {
            std::cout
                << "Usage: algotrading_market_data_service [options]\n"
                << "  --config PATH   default: config/market_data/market_data_config.json\n"
                << "  --run-once      update previous completed BUSINESS UTC day and exit\n";
            std::exit(0);
        }
        else
            throw std::invalid_argument("Unknown option: " + arg);
    }
    return result;
}

std::chrono::year_month_day newestCompletedBusinessUtcDate(const TimeHandler& timeHandler)
{
    return getPreviousDayDate(getCurrentUtcDate(timeHandler.getTime()));
}

std::chrono::system_clock::time_point nextScheduledBusinessRun(
    const TimeHandler& timeHandler,
    int delaySeconds)
{
    return computeNextMidnightUTC(timeHandler.getTime()) + std::chrono::seconds(delaySeconds);
}

// TECHNICAL wait: used only for retry/backoff. It deliberately follows real wall time
// and must not be accelerated by TimeHandler speed.
void interruptibleTechnicalWaitUntil(std::chrono::system_clock::time_point deadline)
{
    while (running.load()) {
        const auto now = std::chrono::system_clock::now();
        if (now >= deadline)
            return;
        const auto remaining = deadline - now;
        const auto chunk = std::min(
            std::chrono::duration_cast<std::chrono::milliseconds>(remaining),
            std::chrono::milliseconds(1000));
        if (chunk.count() > 0)
            std::this_thread::sleep_for(chunk);
    }
}

// BUSINESS wait: correctness is defined by TimeHandler business time. The short real-time
// sleep is only a technical polling cadence so SIGINT/SIGTERM remain responsive; it is not
// an economic timeout and changing TimeHandler speed changes when the business deadline is
// reached without changing this technical cadence.
void interruptibleBusinessWaitUntil(
    const TimeHandler& timeHandler,
    std::chrono::system_clock::time_point businessDeadline)
{
    constexpr auto technicalPollInterval = std::chrono::milliseconds(250);

    while (running.load()) {
        if (timeHandler.getTime() >= businessDeadline)
            return;
        std::this_thread::sleep_for(technicalPollInterval);
    }
}

bool runTarget(
    MarketDataIngestor& ingestor,
    const MarketDataUpdatePublisher& publisher,
    std::chrono::year_month_day targetDate)
{
    try {
        // The ingestor returns only after the SQLite transaction has committed.
        // The notification is intentionally the next step.
        const MarketDataIngestionSummary summary = ingestor.run(targetDate);
        publisher.publish(summary);
        return true;
    }
    catch (const std::exception& error) {
        LG_ERROR(
            "service=market-data event=daily_cycle_failed target_date={} error={}",
            formatYMD(targetDate), error.what()
        );
        return false;
    }
}

} // namespace

int main(int argc, char** argv)
{
    ServiceLogging::setup("market-data");

    try {
        std::signal(SIGINT, stopHandler);
        std::signal(SIGTERM, stopHandler);

        const Options options = parseOptions(argc, argv);
        const MarketDataConfig config = loadMarketDataConfig(options.config);
        const TimeHandlerConfig timeConfig = TimeHandlerFactory::loadConfigFromEnvironment();
        const TimeHandler timeHandler = TimeHandlerFactory::create(timeConfig);

        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
            throw std::runtime_error("curl_global_init failed");

        MarketDataIngestor ingestor(config);
        MarketDataUpdatePublisher publisher(config);

        LG_INFO(
            "service=market-data event=service_ready source=binance storage=sqlite database={} ranking_size={} active_top_n={} retention_days={} minimum_history_days={} nats_url={} stream={} update_subject={} time_speed={} time_identity={}",
            config.database_path.string(), config.ranking_size, config.active_top_n,
            config.retain_after_top_n_days, config.minimum_history_days,
            config.nats_url, config.stream, TransportSubjects::MARKET_DATA_UPDATED,
            timeConfig.speed, timeConfig.identity()
        );
        std::cout.flush();

        // Catch-up is BUSINESS-time based. In normal LIVE the default TimeHandler is identity
        // (speed=1, bias=0), so this remains the previous completed real UTC day. In replay-like
        // configurations it becomes the previous completed simulated/business UTC day.
        auto targetDate = newestCompletedBusinessUtcDate(timeHandler);
        bool success = runTarget(ingestor, publisher, targetDate);

        if (options.run_once) {
            curl_global_cleanup();
            return success ? 0 : 1;
        }

        while (running.load()) {
            if (!success) {
                // Retry/backoff is TECHNICAL time. Do not scale it with replay speed.
                const auto retryAt = std::chrono::system_clock::now() +
                    std::chrono::seconds(config.retry_after_failure_seconds);
                LG_WARN(
                    "service=market-data event=retry_scheduled target_date={} retry_seconds={} time_domain=technical",
                    formatYMD(targetDate), config.retry_after_failure_seconds
                );
                interruptibleTechnicalWaitUntil(retryAt);
                if (!running.load())
                    break;
                success = runTarget(ingestor, publisher, targetDate);
                continue;
            }

            // If ingestion/retry crossed one or more BUSINESS UTC boundaries, observe the newest
            // completed business day immediately instead of sleeping until another boundary.
            const auto newestCompletedDate = newestCompletedBusinessUtcDate(timeHandler);
            if (std::chrono::sys_days{newestCompletedDate} > std::chrono::sys_days{targetDate}) {
                targetDate = newestCompletedDate;
                success = runTarget(ingestor, publisher, targetDate);
                continue;
            }

            const auto scheduled = nextScheduledBusinessRun(
                timeHandler, config.midnight_delay_seconds);
            LG_INFO(
                "service=market-data event=next_daily_run_scheduled business_midnight_delay_seconds={} time_domain=business",
                config.midnight_delay_seconds
            );
            interruptibleBusinessWaitUntil(timeHandler, scheduled);
            if (!running.load())
                break;

            targetDate = newestCompletedBusinessUtcDate(timeHandler);
            success = runTarget(ingestor, publisher, targetDate);
        }

        LG_INFO("service=market-data event=shutdown_complete");
        curl_global_cleanup();
        return 0;
    }
    catch (const std::exception& error) {
        LG_ALERT("service=market-data event=fatal error={}", error.what());
        curl_global_cleanup();
        return 1;
    }
}
