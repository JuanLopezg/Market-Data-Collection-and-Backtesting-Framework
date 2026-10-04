#include <cassert>
#include <string>
#include <type_traits>

#include "venue_capabilities.h"
#include "venue_errors.h"
#include "venue_identity.h"

int main()
{
    using namespace VenueContracts;

    static_assert(std::is_same<VenueId, std::string>::value,
                  "VenueId must remain transport/protocol agnostic");

    VenueContext unspecified;
    assert(!unspecified.valid());

    VenueContext mock{"MOCK", VenueEnvironment::Mock, {}};
    assert(mock.valid());
    assert(std::string(toString(mock.environment)) == "MOCK");

    VenueContext custom{"FUTURE_VENUE", VenueEnvironment::Custom, "SANDBOX"};
    assert(custom.valid());

    VenueContext invalidCustom{"FUTURE_VENUE", VenueEnvironment::Custom, {}};
    assert(!invalidCustom.valid());

    VenueAssetIdentity symbolLeg{"BTC", "BTC-PERP", {}};
    assert(symbolLeg.valid());

    VenueAssetIdentity opaqueLeg{"ASSET_X", {}, "opaque-asset-id"};
    assert(opaqueLeg.valid());

    VenueAssetIdentity unmapped{"ASSET_X", {}, {}};
    assert(!unmapped.valid());

    VenueCapabilitySet capabilities{
        VenueCapability::MarketMetadata,
        VenueCapability::TradingRules,
        VenueCapability::SubmitOrder,
        VenueCapability::CancelOrder,
        VenueCapability::ClientOrderId,
        VenueCapability::AccountSnapshot,
        VenueCapability::OpenOrders,
        VenueCapability::Fills
    };
    assert(capabilities.supports(VenueCapability::SubmitOrder));
    assert(!capabilities.supports(VenueCapability::ModifyOrder));
    assert(capabilities.supportsAll({VenueCapability::SubmitOrder,
                                    VenueCapability::CancelOrder}));
    assert(!capabilities.supportsAll({VenueCapability::SubmitOrder,
                                     VenueCapability::ModifyOrder}));

    VenueError error;
    error.classification = VenueErrorClass::RateLimited;
    error.retryable = true;
    error.native_code = "opaque-code";
    error.native_reason = "opaque native reason";
    assert(std::string(toString(error.classification)) == "RATE_LIMITED");
    assert(error.retryable);
    assert(error.native_code == "opaque-code");

    VenueNativeReferences references;
    references.native_order_id = "native-order-id";
    references.native_fill_id = "native-fill-id";
    references.native_client_order_id = "native-client-id";
    assert(!references.native_order_id.empty());

    return 0;
}
