#include "symbol_map.h"

#include <cmath>
#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace Venue {

SymbolMap::SymbolMap(std::string venue, std::vector<SymbolMapping> entries)
    : venue_(std::move(venue)), entries_(std::move(entries))
{
    if (venue_.empty())
        throw std::invalid_argument("Symbol map venue cannot be empty");
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const SymbolMapping& entry = entries_[i];
        if (entry.source.empty() || entry.asset.empty() || entry.venue_symbol.empty())
            throw std::invalid_argument("Symbol map entry " + std::to_string(i) + " has an empty field");
        if (!std::isfinite(entry.venue_units_per_source_unit) || entry.venue_units_per_source_unit <= 0.0)
            throw std::invalid_argument("Symbol map scale for " + entry.source + " must be positive");
        if (!by_source_.emplace(entry.source, i).second)
            throw std::invalid_argument("Symbol map lists " + entry.source + " twice");
        if (!by_venue_symbol_.emplace(entry.venue_symbol, i).second)
            throw std::invalid_argument("Symbol map maps two source symbols to " + entry.venue_symbol);
    }
}


SymbolMap SymbolMap::load(const std::string& path, const std::string& expectedVenue)
{
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("Cannot read symbol map " + path);
    const nlohmann::json document = nlohmann::json::parse(input, nullptr, false);
    if (document.is_discarded() || !document.is_object())
        throw std::runtime_error("Symbol map " + path + " is not a JSON object");
    if (document.value("schema_version", 0) != 1)
        throw std::runtime_error("Symbol map " + path + " has an unsupported schema_version");
    const std::string venue = document.value("venue", "");
    if (venue != expectedVenue)
        throw std::runtime_error("Symbol map " + path + " is for venue '" + venue + "', expected " + expectedVenue);
    if (!document.contains("symbols") || !document.at("symbols").is_array())
        throw std::runtime_error("Symbol map " + path + " has no symbols list");

    std::vector<SymbolMapping> entries;
    try {
        for (const nlohmann::json& row : document.at("symbols")) {
            entries.push_back({row.at("source").get<std::string>(), row.at("asset").get<std::string>(),
                               row.at("venue_symbol").get<std::string>(),
                               row.at("venue_units_per_source_unit").get<double>()});
        }
    }
    catch (const nlohmann::json::exception&) {
        throw std::runtime_error("Symbol map " + path + " has an entry with a missing or mistyped field");
    }
    return SymbolMap(venue, std::move(entries));
}


const SymbolMapping* SymbolMap::bySource(const Coin& source) const
{
    const auto found = by_source_.find(source);
    return found == by_source_.end() ? nullptr : &entries_[found->second];
}


const SymbolMapping* SymbolMap::byVenueSymbol(const std::string& venueSymbol) const
{
    const auto found = by_venue_symbol_.find(venueSymbol);
    return found == by_venue_symbol_.end() ? nullptr : &entries_[found->second];
}

} // namespace Venue
