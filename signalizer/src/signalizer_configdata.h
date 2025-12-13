#pragma once

#include <string>
#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>

#include "config_data.h"

/**************************************************************************************
 * Purpose : Represents the database-related configuration used by the application.
 *           This configuration is loaded and validated via ConfigData::LoadFromFile(),
 *           then parsed through ParseConfig(). The class provides comparison,
 *           serialization, and accessor utilities.
 **************************************************************************************/
class DatabaseConfig : public ConfigData {
public:
    DatabaseConfig() = default;

    /**************************************************************************************
     * Purpose : Parses the validated JSON configuration and extracts all required fields
     *           for database operation (e.g., exchange name, database path).
     * Args    : j - Validated JSON configuration object.
     * Return  : void
     **************************************************************************************/
    void ParseConfig(const nlohmann::json& j) override;

    /**************************************************************************************
     * Purpose : Compares this configuration with another to determine whether all fields
     *           are identical. Used to avoid unnecessary reloads when the config file
     *           content has not truly changed.
     * Args    : other - Configuration to compare with.
     * Return  : bool - true if both configs contain the same values.
     **************************************************************************************/
    bool operator==(const DatabaseConfig& other) const noexcept;

    /**************************************************************************************
     * Purpose : Serializes the configuration back into JSON. Useful for logging the
     *           active configuration or exporting it for diagnostics.
     * Args    : None
     * Return  : nlohmann::json - JSON representation of this configuration.
     **************************************************************************************/
    nlohmann::json ToJson() const;

private:
    // Exchange identifier used for the database (e.g., "binance").
    std::string main_exchange = "undefined";

    // Filesystem path where the database is located.
    boost::filesystem::path database_path_;

public:
    // Returns the configured main exchange name.
    const std::string& GetMainExchange() const noexcept { return main_exchange; }

    // Returns the filesystem path where the database resides.
    const boost::filesystem::path GetDatabasePath() const noexcept { return database_path_; }
};
