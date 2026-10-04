#include <cassert>
#include <set>
#include <string>

#include "mock_venue_catalog_v1.h"
#include "mock_venue_catalog_version_v1.h"
#include "mock_venue_rules_v1.h"

int main()
{
    using namespace MockVenueV1;

    assert(context().valid());
    assert(context().venue_id == "MOCK");
    assert(context().environment == VenueContracts::VenueEnvironment::Mock);

    const auto& entries = catalog();
    assert(entries.size() == 175U);
    assert(entries.size() == kCatalogEntryCount);

    std::set<std::string> assets;
    std::set<std::string> symbols;
    std::set<std::string> nativeIds;

    for (const auto& entry : entries) {
        assert(entry.valid());
        assert(entry.enabled);
        assert(entry.product_class == VenueContracts::V1::ProductClass::Perpetual);
        assert(entry.settlement_asset == "USD");
        assert(entry.canonical_asset == entry.venue_symbol);
        assert(entry.rule_profile_id == "MOCK_PERP_DEFAULT_V1");
        assert(entry.instrument().valid());
        assert(assets.insert(entry.canonical_asset).second);
        assert(symbols.insert(entry.venue_symbol).second);
        assert(nativeIds.insert(entry.venue_asset_id).second);
    }

    const auto* btc = findByCanonicalAssetExact("BTCUSDT");
    assert(btc != nullptr);
    assert(findByVenueSymbolExact("BTCUSDT") == btc);

    assert(findByCanonicalAssetExact("BTC") == nullptr);
    assert(findByCanonicalAssetExact("btcusdt") == nullptr);
    assert(findByCanonicalAssetExact(" BTCUSDT") == nullptr);
    assert(findByVenueSymbolExact("btcUSDT") == nullptr);

    assert(findByCanonicalAssetExact("2ZUSDT") != nullptr);
    assert(findByCanonicalAssetExact("MARSCOINUSDT") != nullptr);
    assert(findByCanonicalAssetExact("SOONUSDT") != nullptr);
    assert(findByCanonicalAssetExact("USUSDT") != nullptr);
    assert(findByCanonicalAssetExact(u8"龙虾USDT") != nullptr);

    const auto& rules = defaultPerpetualRules();
    assert(rules.valid());
    assert(rules.profile_id == "MOCK_PERP_DEFAULT_V1");
    assert(rules.price_increment == "0.00000001");
    assert(rules.price_scale == 8U);
    assert(rules.size_increment == "0.00000001");
    assert(rules.size_scale == 8U);
    assert(rules.min_size == "0.00000001");
    assert(rules.min_notional == "10.00");
    assert(rules.max_leverage == 20U);
    assert(rules.cross_margin && rules.isolated_margin);
    assert(rules.tif_gtc && rules.tif_ioc);
    assert(rules.post_only && rules.reduce_only && rules.modify);
    assert(rules.client_order_id && rules.native_idempotent_submit);
    assert(rules.reject_nonconforming_precision);

    assert(ruleProfileExact("MOCK_PERP_DEFAULT_V1") == &rules);
    assert(ruleProfileExact("mock_perp_default_v1") == nullptr);
    assert(ruleProfileExact("UNKNOWN") == nullptr);

    assert(isPositiveCanonicalDecimal("10.00"));
    assert(isPositiveCanonicalDecimal("0.00000001"));
    assert(!isPositiveCanonicalDecimal("0"));
    assert(!isPositiveCanonicalDecimal("0.000"));
    assert(!isPositiveCanonicalDecimal(".1"));
    assert(!isPositiveCanonicalDecimal("1."));
    assert(!isPositiveCanonicalDecimal("-1"));
    assert(!isPositiveCanonicalDecimal("1e-8"));

    assert(decimalScale("10.00") == 2U);
    assert(decimalScale("0.00000001") == 8U);

    assert(std::string(kVenueFingerprint).size() == 64U);
    return 0;
}
