#pragma once

#include <memory>
#include <string>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <mutex>
#include <stdexcept>

#include "logger.h"

#include "scheduler.h"

/**************************************************************************************
 * Purpose : A generic configuration auto-reloader that monitors a JSON configuration
 *           file and its schema. When the file changes, a new configuration object is
 *           loaded, validated, parsed, compared with the existing one, and published
 *           as "nextConfig" for consumers (like a DatabaseScheduler).
 *
 * Requirements for TConfig:
 *   - Must inherit from ConfigData
 *   - Must implement: void LoadFromFile(const std::string&, const std::string&)
 *   - Must implement: void ParseConfig(const nlohmann::json&)
 *   - Must implement: bool operator==(const TConfig&) const
 **************************************************************************************/
template<typename TConfig>
class ConfigHandler : public Scheduler<TConfig> {
public:

    /**************************************************************************************
     * Purpose : Builds a ConfigHandler that watches a configuration file and automatically
     *           reloads it when it changes. The scheduler framework handles periodic checks.
     * Args    : configPath       - Path to the JSON config file.
     *           schemaPath       - Path to the JSON schema for validation.
     *           updateRateSeconds - Frequency of checking for file updates.
     * Return  : None
     **************************************************************************************/
    ConfigHandler(const std::string& configPath,
                  const std::string& schemaPath,
                  std::chrono::seconds updateRateSeconds)
        : Scheduler<TConfig>(
              std::shared_ptr<TConfig>{}, // ctx unused in this handler
              std::chrono::duration_cast<std::chrono::milliseconds>(updateRateSeconds),
              std::chrono::milliseconds(500),   // timeout for parsing config
              std::chrono::seconds(0)           // no initial delay
          ),
          configPath(configPath),
          schemaPath(schemaPath),
          updateRate(updateRateSeconds)
    {
        // 1) Load initial configuration
        TConfig initialCfg;
        try {
            initialCfg.LoadFromFile(configPath, schemaPath);
        }
        catch (const std::exception& e) {
            throw std::runtime_error(
                std::string("Initial config invalid: ") + e.what()
            );
        }

        currentConfig = std::make_shared<const TConfig>(std::move(initialCfg));
        nextConfig = nullptr;

        // Store initial write timestamp to avoid immediate reload
        std::error_code ec;
        auto ftime = std::filesystem::last_write_time(configPath, ec);
        if (!ec) {
            using namespace std::chrono;
            lastLoadedTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - decltype(ftime)::clock::now() + std::chrono::system_clock::now()
            );
        }

        LG_INFO("Initial config loaded.");
    }

    /**************************************************************************************
     * Purpose : Returns the current configuration (the one already applied).
     * Args    : None
     * Return  : std::shared_ptr<const TConfig> - Immutable reference to current config.
     **************************************************************************************/
    std::shared_ptr<const TConfig> getCurrentConfig() const {
        return currentConfig;
    }

    /**************************************************************************************
     * Purpose : Retrieves and consumes the next validated configuration (if available).
     * Args    : out - Reference receiving the newly validated configuration.
     * Return  : bool - true if nextConfig existed and was consumed, false otherwise.
     **************************************************************************************/
    bool consumeNextConfig(TConfig& out) {
        std::lock_guard<std::mutex> lock(mtx);
        if (!nextConfig)
            return false;

        out = *nextConfig;
        nextConfig.reset();
        return true;
    }

protected:
    /**************************************************************************************
     * Purpose : Periodically executed by the Scheduler. Detects config file changes,
     *           reloads and validates a new configuration, compares it to the current
     *           one, and if different publishes it to nextConfig.
     * Args    : None
     * Return  : void
     **************************************************************************************/
    void processSecond() override {
        using namespace std::chrono;

        std::error_code ec;
        auto ftime = std::filesystem::last_write_time(configPath, ec);
        if (ec) {
            LG_ERROR("Cannot read last_write_time: {}", ec.message());
            return;
        }

        auto now = system_clock::now();

        // Convert file_clock timestamp to system_clock timestamp
        auto sctp = time_point_cast<system_clock::duration>(
            ftime - decltype(ftime)::clock::now() + system_clock::now()
        );

        if (sctp <= lastLoadedTime) {
            return; // no modification
        }

        LG_INFO("Config file changed, loading...");

        TConfig candidate;
        try {
            candidate.LoadFromFile(configPath, schemaPath);
        }
        catch (const std::exception& e) {
            LG_ERROR("Updated config INVALID: {}", e.what());
            return;
        }

        // Compare with current config
        {
            std::lock_guard<std::mutex> lock(mtx);

            if (currentConfig && *currentConfig == candidate) {
                LG_INFO("Content unchanged. No reload.");
                lastLoadedTime = sctp;
                return;
            }
        }

        // Publish new config
        auto newPtr = std::make_shared<const TConfig>(std::move(candidate));

        {
            std::lock_guard<std::mutex> lock(mtx);
            nextConfig = newPtr;
        }

        lastLoadedTime = sctp;

        LG_INFO("New config validated and ready to apply.");
    }

protected:
    // Current configuration already applied to the system.
    std::shared_ptr<const TConfig> currentConfig;

    // Next configuration waiting to be applied (after validation).
    std::shared_ptr<const TConfig> nextConfig;

private:
    // Path to the JSON configuration file.
    std::string configPath;

    // Path to the JSON schema used for configuration validation.
    std::string schemaPath;

    // Configuration check frequency (informational; scheduler handles timing).
    std::chrono::seconds updateRate;

    // Last time the configuration file was successfully loaded.
    std::chrono::time_point<std::chrono::system_clock> lastLoadedTime =
        std::chrono::system_clock::time_point::min();

    // Mutex protecting access to currentConfig and nextConfig.
    mutable std::mutex mtx;
};
