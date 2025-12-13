#pragma once
#include <string>
#include <nlohmann/json.hpp>

/**************************************************************************************
 * Purpose : Loads a JSON file from disk and parses it into a nlohmann::json object.
 * Args    : path - Path to the JSON file to load.
 * Return  : nlohmann::json - Parsed JSON object.
 *
 * Throws  : std::runtime_error if the file cannot be opened or the JSON cannot be parsed.
 **************************************************************************************/
nlohmann::json LoadJsonFile(const std::string& path);

/**************************************************************************************
 * Purpose : Validates a JSON object using a provided JSON schema.
 * Args    : data   - The JSON object to validate.
 *           schema - A valid JSON schema definition.
 * Return  : void
 *
 * Throws  : std::runtime_error if the JSON object does not conform to the schema.
 **************************************************************************************/
void ValidateJson(const nlohmann::json& data, const nlohmann::json& schema);
