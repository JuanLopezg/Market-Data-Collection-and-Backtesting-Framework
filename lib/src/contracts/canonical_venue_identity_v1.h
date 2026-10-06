#pragma once

#include <string>
#include <utility>

#include "venue_identity.h"

/**************************************************************************************
 * Purpose : Stable v1 market/instrument identity shared by all venue adapters.
 *
 * The base VenueContext identifies venue and environment. This layer adds canonical market
 * and instrument identity without embedding exchange-specific naming rules in business code.
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
