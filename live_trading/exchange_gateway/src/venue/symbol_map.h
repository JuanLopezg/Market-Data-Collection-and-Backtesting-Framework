#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "data_types.h"

// Explicit table from source symbols to one venue's contracts
// (config/venues/<venue>/symbol_map.json, built by tools/kraken_symbol_map.py).
//
// The strategy names assets by their Binance USDT perpetual (BTCUSDT, 1000PEPEUSDT). A venue
// may name and size them differently: Kraken trades PF_XBTUSD in BTC and PF_PEPEUSD in single
// PEPE, while one 1000PEPEUSDT unit is 1000 PEPE. Each entry gives the venue contract and
//     venue quantity = source quantity x venue_units_per_source_unit
//     venue price    = source price    / venue_units_per_source_unit
// so the USD value is the same on both sides. A source symbol missing from the table is not
// tradeable on that venue; nothing is derived from names at runtime.
namespace Venue {

struct SymbolMapping {
    Coin source;              // e.g. "1000PEPEUSDT"
    std::string asset;        // e.g. "PEPE"
    std::string venue_symbol; // e.g. "PF_PEPEUSD"
    double venue_units_per_source_unit = 1.0;
};

class SymbolMap {
public:
    // Rejects empty fields, non-positive scales and duplicate source or venue symbols.
    SymbolMap(std::string venue, std::vector<SymbolMapping> entries);

    // Reads the JSON file and checks that it belongs to expectedVenue (e.g. "kraken-futures").
    static SymbolMap load(const std::string& path, const std::string& expectedVenue);

    const std::string& venue() const { return venue_; }
    const std::vector<SymbolMapping>& entries() const { return entries_; }

    // nullptr when the symbol has no entry.
    const SymbolMapping* bySource(const Coin& source) const;
    const SymbolMapping* byVenueSymbol(const std::string& venueSymbol) const;

private:
    std::string venue_;
    std::vector<SymbolMapping> entries_;
    std::map<Coin, std::size_t> by_source_;
    std::map<std::string, std::size_t> by_venue_symbol_;
};

} // namespace Venue
