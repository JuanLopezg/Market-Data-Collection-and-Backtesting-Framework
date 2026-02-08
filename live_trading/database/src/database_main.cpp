#include <iostream>
#include <fstream>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>
#include <memory>
#include <sqlite3.h>
#include <boost/program_options.hpp>

#include "logger.h"
#include "database_scheduler.h"
#include "config_handler.h"
#include "database_configdata.h"

namespace po = boost::program_options;

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
        /*fileAppender=*/"database.log",
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
    int checkInterval = 30;

    try {
        po::options_description desc("Options");
        desc.add_options()
            ("help,h", "Show help")
            ("debug,d", "Enable debug logging")
            ("config,c", po::value<std::string>(&configPath)->required(), "Path to configuration file")
            ("schema,s", po::value<std::string>(&schemaPath)->required(), "Path to JSON schema file")
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

    // ----------------------------------------------------
    // Create DatabaseScheduler
    // ----------------------------------------------------
    auto databaseScheduler = std::make_unique<DatabaseScheduler>(
        ctx,
        *configHandler,
        std::chrono::milliseconds(1000),   // tick every second
        std::chrono::milliseconds(30000),  // timeout
        std::chrono::seconds(2)            // initial delay
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
