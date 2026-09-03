#include <iostream>
#include <fstream>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>
#include <memory>
#include <stdexcept>
#include <sqlite3.h>
#include <boost/program_options.hpp>

#include "logger.h"
#include "database_scheduler.h"
#include "config_handler.h"
#include "database_configdata.h"
#include "database_downloader.h"
#include "time_utils.h"

namespace po = boost::program_options;

static std::chrono::system_clock::time_point parseClockDate(const std::string& value) {
    if (value.size() != 10 || value[4] != '-' || value[7] != '-')
        throw std::runtime_error("Invalid --clock-date. Expected YYYY-MM-DD");

    int year = std::stoi(value.substr(0, 4));
    unsigned month = static_cast<unsigned>(std::stoi(value.substr(5, 2)));
    unsigned day = static_cast<unsigned>(std::stoi(value.substr(8, 2)));

    std::chrono::year_month_day ymd{
        std::chrono::year{year},
        std::chrono::month{month},
        std::chrono::day{day}
    };

    if (!ymd.ok())
        throw std::runtime_error("Invalid --clock-date. Expected YYYY-MM-DD");

    return std::chrono::sys_days{ymd};
}

void signalHandler(int) {
    Scheduler<DatabaseContext>::globalStop.store(true);
}

int main(int argc, char** argv) {

    // ----------------------------------------------------
    // Logger: inicialización mínima
    // ----------------------------------------------------
    bool debugMode = false;

    // NOTA: El nivel real se ajusta después de parsear argumentos.
    // Aquí ponemos algo básico por si ocurre un error antes.
    Logger::Instance().Setup(
        /*debugEnabled=*/false,
        /*quiet=*/false,
        /*fileAppender=*/"",
        /*rollingAppender=*/"database_roll.log",
        /*includeHeader=*/true
    );

    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);

    // ----------------------------------------------------
    // CLI arguments
    // ----------------------------------------------------
    std::string configPath;
    std::string schemaPath;
    std::string binanceBaseUrl = "https://fapi.binance.com";
    std::string pairSelection = "top50";
    std::string clockDate;
    int checkInterval = 30;
    bool runOnce = false;

    try {
        po::options_description desc("Options");
        desc.add_options()
            ("help,h", "Show help")
            ("debug,d", "Enable debug logging")
            ("config,c", po::value<std::string>(&configPath)->required(), "Path to configuration file")
            ("schema,s", po::value<std::string>(&schemaPath)->required(), "Path to JSON schema file")
            ("binance-base-url", po::value<std::string>(&binanceBaseUrl)->default_value("https://fapi.binance.com"), "Binance-compatible API base URL")
            ("pair-selection", po::value<std::string>(&pairSelection)->default_value("top50"), "Market-data pairs: top50 or all-eligible")
            ("clock-date", po::value<std::string>(&clockDate), "Override UTC clock date for simulation (YYYY-MM-DD)")
            ("run-once", po::bool_switch(&runOnce), "Run one database download cycle and exit")
            ("check-interval,i", po::value<int>(&checkInterval)->default_value(30), "Seconds between configuration checks");

        po::variables_map vm;
        po::store(po::parse_command_line(argc, argv, desc), vm);

        if (vm.count("help")) {
            std::cout << desc << "\n";
            return 0;
        }

        if (vm.count("debug"))
            debugMode = true;

        po::notify(vm);
    }
    catch (const std::exception& e) {
        LG_ERROR(std::string("Argument error: ") + e.what());
        return 1;
    }

    MarketDataPairSelection pairSelectionMode;
    if (pairSelection == "top50")
        pairSelectionMode = MarketDataPairSelection::Top50Volume;
    else if (pairSelection == "all-eligible")
        pairSelectionMode = MarketDataPairSelection::AllEligible;
    else {
        LG_ERROR("Invalid --pair-selection '{}'. Use top50 or all-eligible.", pairSelection);
        return 1;
    }

    // ----------------------------------------------------
    // Set log level based on CLI flag
    // ----------------------------------------------------
    Logger::Instance().Setup(
        /*debugEnabled=*/debugMode,
        /*quiet=*/false,
        /*fileAppender=*/"database.log",
        /*rollingAppender=*/"database_roll.log",
        /*includeHeader=*/true
    );

    if (debugMode)
        LG_DEBUG("Debug mode ENABLED.");
    else
        LG_INFO("Debug mode disabled.");

    LG_INFO("Starting Database service...");

    // ----------------------------------------------------
    // Create ConfigHandler
    // ----------------------------------------------------
    auto configHandler = std::make_unique<DatabaseConfigHandler>(
        configPath,
        schemaPath,
        std::chrono::seconds(checkInterval)
    );

    // ----------------------------------------------------
    // Load initial config into context
    // ----------------------------------------------------
    auto ctx = std::make_shared<DatabaseContext>();
    if (auto initialCfg = configHandler->getCurrentConfig()) {
        ctx->config = *initialCfg;
    } else {
        LG_ERROR("No initial config available from ConfigHandler.");
        return 1;
    }

    std::shared_ptr<Clock> clock = std::make_shared<SystemClock>();
    if (!clockDate.empty()) {
        try {
            clock = std::make_shared<FixedClock>(parseClockDate(clockDate));
            LG_INFO("Using fixed simulation clock date: {} 00:00 UTC", clockDate);
        }
        catch (const std::exception& e) {
            LG_ERROR("Invalid --clock-date '{}': {}", clockDate, e.what());
            return 1;
        }
    }

    if (runOnce) {
        DatabaseDownloader downloader(
            ctx->config.GetDatabasePath(),
            binanceBaseUrl,
            pairSelectionMode);

        auto targetDate = getPreviousDayDate(getCurrentUtcDate(clock->now()));
        if (!downloader.downloadData(targetDate)) {
            LG_ERROR("Run-once database update failed.");
            return 1;
        }

        LG_INFO("Run-once database update completed successfully.");
        return 0;
    }

    // ----------------------------------------------------
    // Create DatabaseScheduler
    // ----------------------------------------------------
    auto databaseScheduler = std::make_unique<DatabaseScheduler>(
        ctx,
        *configHandler,
        std::chrono::milliseconds(1000),   // tick every second
        std::chrono::milliseconds(30000),  // timeout
        std::chrono::seconds(2),           // initial delay
        clock,
        binanceBaseUrl,
        pairSelectionMode
    );

    // Start config handler thread
    configHandler->startAsync();

    LG_INFO("Running... Press CTRL+C to stop.");

    // ----------------------------------------------------
    // Run scheduler (blocking)
    // ----------------------------------------------------
    databaseScheduler->start();

    // ----------------------------------------------------
    // Graceful shutdown
    // ----------------------------------------------------
    configHandler->stop();
    LG_INFO("Shutting down Database application...");

    return 0;
}
