#pragma once

#include <string>
#include <utility>

/**************************************************************************************
 * Header  : venue_identity.h
 * Step    : 47A — Canonical Multi-Exchange Architecture Baseline
 * Purpose : Venue-neutral identity vocabulary shared by future concrete adapters
 *
 * This header contains only canonical identity concepts. Concrete protocol, credentials
 * and venue-specific semantics belong to the adapter edge rather than the trading core.
 **************************************************************************************/
namespace VenueContracts {

using VenueId = std::string;

enum class VenueEnvironment {
    Unspecified = 0,
    Mock,
    Testnet,
    Mainnet,
    Custom
};

struct VenueContext {
    VenueId venue_id;
    VenueEnvironment environment = VenueEnvironment::Unspecified;

    // Required only when environment == Custom. Examples are intentionally not encoded
    // here because environment naming belongs to the concrete adapter/deployment.
    std::string custom_environment;

    bool valid() const
    {
        if (venue_id.empty() || environment == VenueEnvironment::Unspecified)
            return false;
        if (environment == VenueEnvironment::Custom)
            return !custom_environment.empty();
        return custom_environment.empty();
    }
};

/**************************************************************************************
 * Type    : VenueAssetIdentity
 * Purpose : Explicit source-independent mapping from one canonical/internal asset to
 *           one venue-native locator.
 *
 * No symbol stripping, case conversion or alias guessing is allowed by this contract.
 * venue_symbol is the human/API symbol when a venue exposes one; venue_asset_id is the
 * opaque/native identifier when a venue requires one. At least one native locator must
 * be present before routing can be considered mapped.
 **************************************************************************************/
struct VenueAssetIdentity {
    std::string canonical_asset;
    std::string venue_symbol;
    std::string venue_asset_id;

    bool valid() const
    {
        return !canonical_asset.empty() && (!venue_symbol.empty() || !venue_asset_id.empty());
    }
};

/**************************************************************************************
 * Type    : VenueNativeReferences
 * Purpose : Preserve venue-native identifiers as audit/reconciliation evidence without
 *           promoting any one venue's identifier format into canonical core types.
 **************************************************************************************/
struct VenueNativeReferences {
    std::string native_order_id;
    std::string native_fill_id;
    std::string native_client_order_id;
};

inline const char* toString(VenueEnvironment value)
{
    switch (value) {
    case VenueEnvironment::Unspecified: return "UNSPECIFIED";
    case VenueEnvironment::Mock: return "MOCK";
    case VenueEnvironment::Testnet: return "TESTNET";
    case VenueEnvironment::Mainnet: return "MAINNET";
    case VenueEnvironment::Custom: return "CUSTOM";
    }
    return "UNSPECIFIED";
}

} // namespace VenueContracts
