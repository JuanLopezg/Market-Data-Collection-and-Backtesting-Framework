#pragma once

#include <string>
#include <nlohmann/json.hpp>

#include "json_utils.h"   // JSON loading + validation utilities

/**************************************************************************************
 * Purpose : Abstract base class representing configuration data. Provides a unified
 *           mechanism for loading, validating, and parsing configuration files based
 *           on JSON + JSON schema. Derived types must implement ParseConfig().
 **************************************************************************************/
class ConfigData {
public:
    virtual ~ConfigData() = default;

    /**************************************************************************************
     * Purpose : Loads and validates a configuration file using a JSON schema, then
     *           delegates the actual parsing to the derived class via ParseConfig().
     * Args    : configPath - Path to the JSON config file.
     *           schemaPath - Path to the JSON schema file.
     * Return  : void
     *
     * Throws  : std::runtime_error if loading or validation fails.
     **************************************************************************************/
    void LoadFromFile(const std::string& configPath,
                      const std::string& schemaPath)
    {
        nlohmann::json configJson = LoadJsonFile(configPath);
        nlohmann::json schemaJson = LoadJsonFile(schemaPath);

        ValidateJson(configJson, schemaJson);

        ParseConfig(configJson);
    }

protected:
    /**************************************************************************************
     * Purpose : Implemented by derived configuration classes to extract and validate
     *           required fields from the parsed JSON object.
     * Args    : j - JSON object representing the validated configuration.
     * Return  : void
     **************************************************************************************/
    virtual void ParseConfig(const nlohmann::json& j) = 0;
};
