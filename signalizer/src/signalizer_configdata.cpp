#include "database_configdata.h"
#include <stdexcept>

/**************************************************************************************
 * Purpose : Extracts database-specific configuration fields from the validated JSON
 *           object. Ensures required fields exist and contain valid data, otherwise
 *           throws an exception. Responsible for populating internal configuration
 *           attributes such as main_exchange and database_path_.
 * Args    : j - Validated JSON configuration object.
 * Return  : void
 **************************************************************************************/
void DatabaseConfig::ParseConfig(const nlohmann::json& j)
{
    // Validate and extract main_exchange
    if (!j.contains("main_exchange") || !j["main_exchange"].is_string()) {
        throw std::runtime_error("'main_exchange' must be a non-empty string");
    }

    main_exchange = j["main_exchange"].get<std::string>();
    if (main_exchange.empty()) {
        throw std::runtime_error("'main_exchange' cannot be empty");
    }

    // Validate and extract database_path
    if (!j.contains("database_path") || !j["database_path"].is_string()) {
        throw std::runtime_error("'database_path' must be a valid file path string");
    }

    database_path_ = boost::filesystem::path(j["database_path"].get<std::string>());
}


/**************************************************************************************
 * Purpose : Compares this configuration with another configuration object to check
 *           whether they contain identical values. Used by ConfigHandler to determine
 *           whether an update is necessary.
 * Args    : other - Another DatabaseConfig instance to compare with.
 * Return  : bool - true if both instances contain the same configuration values.
 **************************************************************************************/
bool DatabaseConfig::operator==(const DatabaseConfig& other) const noexcept
{
    return main_exchange == other.main_exchange &&
           database_path_ == other.database_path_;
}


/**************************************************************************************
 * Purpose : Serializes the configuration fields into a JSON object. Useful for logging,
 *           debugging, or exporting the currently active configuration.
 * Args    : None
 * Return  : nlohmann::json - JSON object containing all configuration fields.
 **************************************************************************************/
nlohmann::json DatabaseConfig::ToJson() const
{
    return nlohmann::json{
        {"main_exchange", main_exchange},
        {"database_path", database_path_.string()}
    };
}
