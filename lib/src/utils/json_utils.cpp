#include "json_utils.h"
#include <fstream>
#include <stdexcept>
#include <nlohmann/json-schema.hpp>
#include <chrono>
#include <ctime>
#include <fmt/format.h>
#include <fmt/chrono.h>
#include <fstream>
#include <stdexcept>
#include <string>
#include <fmt/core.h>
#include <fmt/chrono.h>

/**************************************************************************************
 * Purpose : Loads a JSON file from disk and parses it into a nlohmann::json object.
 * Args    : path - Path to the JSON file to load.
 * Return  : nlohmann::json - Parsed JSON object.
 *
 * Throws  : std::runtime_error if the file cannot be opened or the JSON cannot be parsed.
 **************************************************************************************/
nlohmann::json LoadJsonFile(const std::string& path)
{
    std::ifstream f(path);
    if (!f.is_open()) {
        throw std::runtime_error("cannot open json file: " + path);
    }

    try {
        nlohmann::json j;
        f >> j;
        return j;
    }
    catch (const std::exception& e) {
        throw std::runtime_error(
            std::string("failed to parse JSON: ") + e.what()
        );
    }
}


/**************************************************************************************
 * Purpose : Validates a JSON object using a provided JSON schema.
 * Args    : data   - The JSON object to validate.
 *           schema - A valid JSON schema definition.
 * Return  : void
 *
 * Throws  : std::runtime_error if the JSON object does not conform to the schema.
 **************************************************************************************/
void ValidateJson(const nlohmann::json& data,
                  const nlohmann::json& schema)
{
    try {
        nlohmann::json_schema::json_validator validator;
        validator.set_root_schema(schema);
        validator.validate(data);
    }
    catch (const std::exception& e) {
        throw std::runtime_error(
            std::string("JSON validation failed: ") + e.what()
        );
    }
}
