#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "canonical_venue_identity_v1.h"
#include "mock_venue_catalog_version_v1.h"
#include "mock_venue_rules_v1.h"

namespace MockVenueV1 {

struct CatalogEntry {
    std::string canonical_asset;
    VenueContracts::V1::ProductClass product_class =
        VenueContracts::V1::ProductClass::Unspecified;
    std::string settlement_asset;
    std::string venue_symbol;
    std::string venue_asset_id;
    std::string rule_profile_id;
    bool enabled = false;

    bool valid() const
    {
        return !canonical_asset.empty() &&
               product_class != VenueContracts::V1::ProductClass::Unspecified &&
               !settlement_asset.empty() &&
               !venue_symbol.empty() &&
               !venue_asset_id.empty() &&
               ruleProfileExact(rule_profile_id) != nullptr;
    }

    VenueContracts::V1::InstrumentIdentity instrument() const
    {
        VenueContracts::V1::InstrumentIdentity out;
        out.market.canonical_asset = canonical_asset;
        out.market.product_class = product_class;
        out.market.quote_or_settlement_asset = settlement_asset;
        out.venue_symbol = venue_symbol;
        out.venue_asset_id = venue_asset_id;
        return out;
    }
};

inline VenueContracts::V1::VenueContext context()
{
    return VenueContracts::V1::VenueContext{
        "MOCK",
        VenueContracts::VenueEnvironment::Mock,
        {}
    };
}

inline const std::vector<CatalogEntry>& catalog()
{
    static const std::vector<CatalogEntry> entries = {
#include "mock_venue_catalog_data_v1.inc"
    };
    return entries;
}

inline const CatalogEntry* findByCanonicalAssetExact(const std::string& canonical_asset)
{
    const auto& entries = catalog();
    const auto it = std::find_if(entries.begin(), entries.end(),
        [&](const CatalogEntry& entry) {
            return entry.canonical_asset == canonical_asset;
        });
    return it == entries.end() ? nullptr : &*it;
}

inline const CatalogEntry* findByVenueSymbolExact(const std::string& venue_symbol)
{
    const auto& entries = catalog();
    const auto it = std::find_if(entries.begin(), entries.end(),
        [&](const CatalogEntry& entry) {
            return entry.venue_symbol == venue_symbol;
        });
    return it == entries.end() ? nullptr : &*it;
}

inline const RuleProfile* rulesForExact(const CatalogEntry& entry)
{
    return entry.enabled ? ruleProfileExact(entry.rule_profile_id) : nullptr;
}

} // namespace MockVenueV1
