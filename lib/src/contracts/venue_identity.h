#pragma once

#include <string>
#include <utility>

// Shared venue vocabulary and the versioned adapter contract. The V1 namespace records the contract format.

// Venue-neutral identity vocabulary shared by future concrete adapters
//
// This header contains only canonical identity concepts. Concrete protocol, credentials
// and venue-specific semantics belong to the adapter edge rather than the trading core.
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

// Explicit source-independent mapping from one canonical/internal asset to
// one venue-native locator.
//
// No symbol stripping, case conversion or alias guessing is allowed by this contract.
// venue_symbol is the human/API symbol when a venue exposes one; venue_asset_id is the
// opaque/native identifier when a venue requires one. At least one native locator must
// be present before routing can be considered mapped.
struct VenueAssetIdentity {
    std::string canonical_asset;
    std::string venue_symbol;
    std::string venue_asset_id;

    bool valid() const
    {
        return !canonical_asset.empty() && (!venue_symbol.empty() || !venue_asset_id.empty());
    }
};

// Preserve venue-native identifiers as audit/reconciliation evidence without
// promoting any one venue's identifier format into canonical core types.
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

// Stable v1 market/instrument identity shared by all venue adapters.
//
// The base VenueContext identifies venue and environment. This layer adds canonical market
// and instrument identity without embedding exchange-specific naming rules in business code.
namespace VenueContracts {
namespace V1 {

using VenueId = VenueContracts::VenueId;
using VenueEnvironment = VenueContracts::VenueEnvironment;
using VenueContext = VenueContracts::VenueContext;
using NativeReferences = VenueContracts::VenueNativeReferences;

enum class ProductClass {
    Unspecified = 0,
    Perpetual,
    Spot,
    Outcome,
    Other
};

struct MarketIdentity {
    std::string canonical_asset;
    ProductClass product_class = ProductClass::Unspecified;

    // Optional canonical disambiguators. They are not venue symbols.
    std::string quote_or_settlement_asset;
    std::string contract_variant;

    bool valid() const
    {
        return !canonical_asset.empty() && product_class != ProductClass::Unspecified;
    }
};

struct InstrumentIdentity {
    MarketIdentity market;

    // Explicit native locator(s). No stripping/case conversion/alias guessing.
    std::string venue_symbol;
    std::string venue_asset_id;

    bool valid() const
    {
        return market.valid() && (!venue_symbol.empty() || !venue_asset_id.empty());
    }
};

inline const char* toString(ProductClass value)
{
    switch (value) {
    case ProductClass::Unspecified: return "UNSPECIFIED";
    case ProductClass::Perpetual: return "PERPETUAL";
    case ProductClass::Spot: return "SPOT";
    case ProductClass::Outcome: return "OUTCOME";
    case ProductClass::Other: return "OTHER";
    }
    return "UNSPECIFIED";
}

} // namespace V1
} // namespace VenueContracts
