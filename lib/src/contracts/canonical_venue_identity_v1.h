#pragma once

#include <string>
#include <utility>

#include "venue_identity.h"

/**************************************************************************************
 * Header  : canonical_venue_identity_v1.h
 * Step    : 48 — Canonical Multi-Venue Adapter Contract v1
 * Purpose : Stable v1 market/instrument identity used by all concrete VenueAdapters
 *
 * Step47A's venue_identity.h remains the frozen architecture-baseline artifact.
 * This v1 layer builds on its VenueContext while adding the product/market identity
 * frozen by Step47C. It intentionally contains no concrete exchange/protocol names.
 **************************************************************************************/
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
