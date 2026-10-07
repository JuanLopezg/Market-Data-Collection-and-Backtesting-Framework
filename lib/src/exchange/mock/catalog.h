#pragma once

#include <cstddef>
#include <algorithm>
#include <string>
#include <vector>
#include "venue_identity.h"
#include "rules.h"

// Frozen market catalog and its accepted data hashes. Version strings are evidence, not source-file names.

namespace MockVenue {

inline constexpr const char* kCatalogVersion = "mock-catalog-v1-step49";
inline constexpr const char* kRulesVersion = "mock-rules-v1-step49";
inline constexpr const char* kSourceRegistryVersion = "2026-09-27-step37a.5";
inline constexpr const char* kCatalogSha256 = "1b7b0f680df740b864086e5d124f38d297a87b35a19564d706a88ede8278f70c";
inline constexpr const char* kRulesSha256 = "170ffa48e9e7d7e3bc43e3658f41c7da7273e45721f6140382869d481f4c2ab3";
inline constexpr const char* kSourceUniverseSha256 = "e454852e189a449417757a18f0e782ce27083f7c07b843050f2eb975b18196a0";
inline constexpr const char* kVenueFingerprint = "a6d2b98a45d11e57bc0d649ff9569952c73adff1671608f5eafe561169ccbb27";
inline constexpr std::size_t kCatalogEntryCount = 175U;

} // namespace MockVenue

namespace MockVenue {

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

// catalog_data.inc is frozen evidence data. Keep entries, order and accepted hashes
// synchronized through catalog validation rather than editing them for readability.
inline const std::vector<CatalogEntry>& catalog()
{
    static const std::vector<CatalogEntry> entries = {
#include "catalog_data.inc"
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

} // namespace MockVenue
