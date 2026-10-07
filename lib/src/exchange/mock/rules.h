// Exact instrument precision, minimum size/notional and supported order features.
// Decimal increments remain text so binary floating-point does not redefine the venue grid.

#pragma once

#include <cstddef>
#include <string>

#include "venue_identity.h"

namespace MockVenue {

inline bool isPositiveCanonicalDecimal(const std::string& value)
{
    if (value.empty())
        return false;

    bool saw_digit = false;
    bool saw_dot = false;
    bool saw_nonzero = false;

    for (std::size_t i = 0; i < value.size(); ++i) {
        const char ch = value[i];
        if (ch == '.') {
            if (saw_dot || i == 0 || i + 1 == value.size())
                return false;
            saw_dot = true;
            continue;
        }
        if (ch < '0' || ch > '9')
            return false;
        saw_digit = true;
        if (ch != '0')
            saw_nonzero = true;
    }
    return saw_digit && saw_nonzero;
}

inline std::size_t decimalScale(const std::string& value)
{
    const auto pos = value.find('.');
    return pos == std::string::npos ? 0U : value.size() - pos - 1U;
}

struct RuleProfile {
    std::string profile_id;
    VenueContracts::V1::ProductClass product_class =
        VenueContracts::V1::ProductClass::Unspecified;
    std::string settlement_asset;

    std::string price_increment;
    std::size_t price_scale = 0;
    std::string size_increment;
    std::size_t size_scale = 0;
    std::string min_size;
    std::string min_notional;

    unsigned int max_leverage = 0;
    bool cross_margin = false;
    bool isolated_margin = false;
    bool tif_gtc = false;
    bool tif_ioc = false;
    bool post_only = false;
    bool reduce_only = false;
    bool modify = false;
    bool client_order_id = false;
    bool native_idempotent_submit = false;
    bool reject_nonconforming_precision = true;
    bool enabled = false;

    bool valid() const
    {
        return !profile_id.empty() &&
               product_class != VenueContracts::V1::ProductClass::Unspecified &&
               !settlement_asset.empty() &&
               isPositiveCanonicalDecimal(price_increment) &&
               decimalScale(price_increment) == price_scale &&
               isPositiveCanonicalDecimal(size_increment) &&
               decimalScale(size_increment) == size_scale &&
               isPositiveCanonicalDecimal(min_size) &&
               isPositiveCanonicalDecimal(min_notional) &&
               max_leverage > 0 &&
               (cross_margin || isolated_margin) &&
               tif_gtc &&
               enabled;
    }
};

inline const RuleProfile& defaultPerpetualRules()
{
    static const RuleProfile rules = [] {
        RuleProfile r;
        r.profile_id = "MOCK_PERP_DEFAULT_V1";
        r.product_class = VenueContracts::V1::ProductClass::Perpetual;
        r.settlement_asset = "USD";
        r.price_increment = "0.00000001";
        r.price_scale = 8U;
        r.size_increment = "0.00000001";
        r.size_scale = 8U;
        r.min_size = "0.00000001";
        r.min_notional = "10.00";
        r.max_leverage = 20U;
        r.cross_margin = true;
        r.isolated_margin = true;
        r.tif_gtc = true;
        r.tif_ioc = true;
        r.post_only = true;
        r.reduce_only = true;
        r.modify = true;
        r.client_order_id = true;
        r.native_idempotent_submit = true;
        r.reject_nonconforming_precision = true;
        r.enabled = true;
        return r;
    }();
    return rules;
}

inline const RuleProfile* ruleProfileExact(const std::string& profile_id)
{
    const auto& rules = defaultPerpetualRules();
    return profile_id == rules.profile_id ? &rules : nullptr;
}

} // namespace MockVenue
