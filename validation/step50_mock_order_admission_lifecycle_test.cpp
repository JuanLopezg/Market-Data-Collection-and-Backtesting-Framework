#include <cassert>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "mock_order_admission_lifecycle_v1.h"
#include "mock_venue_catalog_v1.h"

using namespace VenueContracts::V1;
using namespace MockVenueV1;

namespace {

RequestIdentity req(const std::string& id, Timestamp ts)
{
    RequestIdentity r;
    r.venue = context();
    r.request_id = id;
    r.correlation_id = "corr-" + id;
    r.requested_at = ts;
    return r;
}

LimitOrderIntent makeOrder(
    OrderID id,
    const std::string& asset,
    double quantity,
    double price,
    const std::string& cloid = {},
    TimeInForce tif = TimeInForce::Gtc,
    bool post_only = false,
    bool reduce_only = false)
{
    const auto* entry = findByCanonicalAssetExact(asset);
    assert(entry != nullptr);

    LimitOrderIntent o;
    o.local_order_id = id;
    o.strategy_id = 1;
    o.created_at = 100;
    o.active_from = 101;
    o.instrument = entry->instrument();
    o.side = Side::Buy;
    o.quantity = quantity;
    o.limit_price = price;
    o.time_in_force = tif;
    o.post_only = post_only;
    o.reduce_only = reduce_only;
    o.client_order_id = cloid;
    return o;
}

SubmitOrderBatch submitOne(
    const std::string& request_id,
    const std::string& item_id,
    const LimitOrderIntent& o,
    Timestamp ts)
{
    SubmitOrderBatch b;
    b.request = req(request_id, ts);
    b.items.push_back(SubmitOrderItem{item_id, o});
    return b;
}

CancelOrderBatch cancelOne(
    const std::string& request_id,
    const std::string& item_id,
    const StoredOrder& stored,
    Timestamp ts)
{
    CancelOrderBatch b;
    b.request = req(request_id, ts);
    CancelOrderItem i;
    i.item_id = item_id;
    i.order.local_order_id = stored.intent.local_order_id;
    i.order.instrument = stored.intent.instrument;
    i.order.native_references = stored.native_references;
    b.items.push_back(i);
    return b;
}

ModifyOrderBatch modifyOne(
    const std::string& request_id,
    const std::string& item_id,
    const StoredOrder& stored,
    const LimitOrderIntent& replacement,
    Timestamp ts)
{
    ModifyOrderBatch b;
    b.request = req(request_id, ts);
    ModifyOrderItem i;
    i.item_id = item_id;
    i.order.local_order_id = stored.intent.local_order_id;
    i.order.instrument = stored.intent.instrument;
    i.order.native_references = stored.native_references;
    i.replacement = replacement;
    b.items.push_back(i);
    return b;
}

const OperationResult& operationAt(const std::vector<Event>& e, std::size_t i)
{
    assert(i < e.size() && std::holds_alternative<OperationResult>(e[i]));
    return std::get<OperationResult>(e[i]);
}

const OrderUpdate& updateAt(const std::vector<Event>& e, std::size_t i)
{
    assert(i < e.size() && std::holds_alternative<OrderUpdate>(e[i]));
    return std::get<OrderUpdate>(e[i]);
}

} // namespace

int main()
{
    MockOrderAdmissionLifecycleV1 engine;

    const auto caps = MockOrderAdmissionLifecycleV1::capabilities();
    assert(caps.supports(Capability::SubmitOrder));
    assert(caps.supports(Capability::CancelOrder));
    assert(caps.supports(Capability::ModifyOrder));
    assert(caps.supports(Capability::IdempotentSubmit));
    assert(!caps.supports(Capability::Fills));
    assert(!caps.supports(Capability::AccountSnapshot));

    std::uint64_t units = 0;
    assert(doubleToScaledUnitsExact(0.1, 8U, &units));
    assert(units == 10000000ULL);
    assert(divisibleByIncrement(50000.12345678, "0.00000001", 8U));
    assert(!divisibleByIncrement(50000.000000005, "0.00000001", 8U));
    assert(!divisibleByIncrement(0.000000015, "0.00000001", 8U));

    // Valid submit -> result then ACCEPTED.
    const auto btc = makeOrder(1001, "BTCUSDT", 0.001, 50000.0, "cloid-btc");
    engine.submit(submitOne("submit-btc", "btc-item", btc, 200));
    assert(engine.orders().size() == 1U);
    assert(engine.emittedEvents().size() == 2U);
    assert(operationAt(engine.emittedEvents(), 0).item_results[0].accepted);
    assert(updateAt(engine.emittedEvents(), 1).status == OrderLifecycleStatus::Accepted);

    // No economic execution events.
    for (const auto& e : engine.emittedEvents()) {
        assert(!std::holds_alternative<Fill>(e));
        assert(!std::holds_alternative<AccountingEvent>(e));
        assert(!std::holds_alternative<AccountSnapshot>(e));
    }

    // Same request + same payload: cached result only.
    engine.clearEmittedEvents();
    engine.submit(submitOne("submit-btc", "btc-item", btc, 200));
    assert(engine.orders().size() == 1U);
    assert(engine.emittedEvents().size() == 1U);
    assert(operationAt(engine.emittedEvents(), 0).item_results[0].accepted);

    // Same request + different payload: operation duplicate.
    engine.clearEmittedEvents();
    auto changed = btc;
    changed.limit_price = 50001.0;
    engine.submit(submitOne("submit-btc", "btc-item", changed, 200));
    const auto& dupReq = operationAt(engine.emittedEvents(), 0);
    assert(dupReq.scope == ResultScope::Operation);
    assert(dupReq.operation_error.classification == ErrorClass::DuplicateRequest);

    // Exact identity mismatch.
    engine.clearEmittedEvents();
    auto wrongLocator = makeOrder(1002, "ETHUSDT", 0.01, 3000.0, "cloid-wrong");
    wrongLocator.instrument.venue_asset_id = "guessed-native-id";
    engine.submit(submitOne("wrong-locator", "wrong", wrongLocator, 210));
    assert(operationAt(engine.emittedEvents(), 0).item_results[0].error.classification ==
           ErrorClass::UnknownAsset);

    // Price grid.
    engine.clearEmittedEvents();
    auto offPrice = makeOrder(1003, "ETHUSDT", 0.01, 3000.000000005, "cloid-off-price");
    engine.submit(submitOne("off-price", "off-price-item", offPrice, 211));
    assert(operationAt(engine.emittedEvents(), 0).item_results[0].error.classification ==
           ErrorClass::InvalidPriceIncrement);

    // Size grid.
    engine.clearEmittedEvents();
    auto offSize = makeOrder(1004, "ETHUSDT", 0.000000015, 3000.0, "cloid-off-size");
    engine.submit(submitOne("off-size", "off-size-item", offSize, 212));
    assert(operationAt(engine.emittedEvents(), 0).item_results[0].error.classification ==
           ErrorClass::InvalidQuantityIncrement);

    // Minimum notional.
    engine.clearEmittedEvents();
    auto belowMin = makeOrder(1005, "ETHUSDT", 0.001, 3000.0, "cloid-min");
    engine.submit(submitOne("below-min", "below-min-item", belowMin, 213));
    assert(operationAt(engine.emittedEvents(), 0).item_results[0].error.classification ==
           ErrorClass::BelowMinimum);

    // post-only + IOC conflict.
    engine.clearEmittedEvents();
    auto poIoc = makeOrder(
        1006, "ETHUSDT", 0.01, 3000.0, "cloid-po-ioc",
        TimeInForce::Ioc, true, false);
    engine.submit(submitOne("po-ioc", "po-ioc-item", poIoc, 214));
    assert(operationAt(engine.emittedEvents(), 0).item_results[0].error.classification ==
           ErrorClass::InvalidRequest);

    // Mixed batch: valid item accepted, invalid exact mapping rejected.
    engine.clearEmittedEvents();
    SubmitOrderBatch mixed;
    mixed.request = req("mixed", 220);
    mixed.items.push_back(SubmitOrderItem{
        "eth-good",
        makeOrder(2001, "ETHUSDT", 0.01, 3000.0, "cloid-eth")
    });
    auto bad = makeOrder(2002, "SOLUSDT", 0.1, 150.0, "cloid-bad");
    bad.instrument.market.canonical_asset = "SOL";
    mixed.items.push_back(SubmitOrderItem{"sol-bad", bad});
    engine.submit(mixed);

    const auto& mixedResult = operationAt(engine.emittedEvents(), 0);
    assert(mixedResult.item_results.size() == 2U);
    assert(mixedResult.item_results[0].accepted);
    assert(!mixedResult.item_results[1].accepted);
    assert(mixedResult.item_results[1].error.classification == ErrorClass::UnknownAsset);
    assert(updateAt(engine.emittedEvents(), 1).local_order_id == 2001U);
    assert(engine.findOrder(2001) != nullptr);
    assert(engine.findOrder(2002) == nullptr);

    // Step51 bridge: ACCEPTED -> RESTING.
    engine.clearEmittedEvents();
    assert(engine.applyExternalLifecycleTransition(
        1001, 230, OrderLifecycleStatus::Resting,
        0.0, 0.001, "MATCHER_RESTING"));
    assert(engine.findOrder(1001)->status == OrderLifecycleStatus::Resting);
    assert(updateAt(engine.emittedEvents(), 0).status == OrderLifecycleStatus::Resting);

    // Modify re-runs admission and preserves native order id + current lifecycle phase.
    engine.clearEmittedEvents();
    const StoredOrder beforeModify = *engine.findOrder(1001);
    auto replacement = beforeModify.intent;
    replacement.limit_price = 49999.0;
    replacement.client_order_id = "cloid-btc-v2";
    engine.modify(modifyOne(
        "modify-btc", "modify-btc-item", beforeModify, replacement, 240));
    assert(operationAt(engine.emittedEvents(), 0).item_results[0].accepted);
    assert(updateAt(engine.emittedEvents(), 1).status == OrderLifecycleStatus::Resting);

    const StoredOrder* afterModify = engine.findOrder(1001);
    assert(afterModify != nullptr);
    assert(afterModify->intent.limit_price == 49999.0);
    assert(afterModify->native_references.native_order_id ==
           beforeModify.native_references.native_order_id);

    // Historical client ID cannot be reused.
    engine.clearEmittedEvents();
    auto dupCloid = makeOrder(3001, "SOLUSDT", 0.1, 150.0, "cloid-btc");
    engine.submit(submitOne("dup-cloid", "dup-cloid-item", dupCloid, 241));
    assert(operationAt(engine.emittedEvents(), 0).item_results[0].error.classification ==
           ErrorClass::DuplicateRequest);

    // Cancel -> result, CANCEL_PENDING, CANCELED.
    engine.clearEmittedEvents();
    const StoredOrder beforeCancel = *engine.findOrder(2001);
    engine.cancel(cancelOne(
        "cancel-eth", "cancel-eth-item", beforeCancel, 250));
    assert(engine.emittedEvents().size() == 3U);
    assert(operationAt(engine.emittedEvents(), 0).item_results[0].accepted);
    assert(updateAt(engine.emittedEvents(), 1).status == OrderLifecycleStatus::CancelPending);
    assert(updateAt(engine.emittedEvents(), 2).status == OrderLifecycleStatus::Canceled);
    assert(engine.findOrder(2001)->status == OrderLifecycleStatus::Canceled);

    // Terminal order cannot be canceled again or resurrected.
    engine.clearEmittedEvents();
    engine.cancel(cancelOne(
        "cancel-eth-again", "cancel-again-item",
        *engine.findOrder(2001), 251));
    assert(operationAt(engine.emittedEvents(), 0).item_results[0].error.classification ==
           ErrorClass::OrderNotFound);

    assert(!engine.applyExternalLifecycleTransition(
        2001, 252, OrderLifecycleStatus::Resting,
        0.0, 0.01, "INVALID_RESURRECTION"));

    // Wrong venue context -> operation-level fail-closed.
    engine.clearEmittedEvents();
    auto wrongVenue = submitOne(
        "wrong-venue", "wrong-venue-item",
        makeOrder(4001, "BTCUSDT", 0.001, 50000.0, "cloid-wv"), 260);
    wrongVenue.request.venue.venue_id = "FUTURE_VENUE";
    wrongVenue.request.venue.environment = VenueContracts::VenueEnvironment::Custom;
    wrongVenue.request.venue.custom_environment = "SANDBOX";
    engine.submit(wrongVenue);

    const auto& wrongVenueResult = operationAt(engine.emittedEvents(), 0);
    assert(wrongVenueResult.scope == ResultScope::Operation);
    assert(wrongVenueResult.operation_error.classification == ErrorClass::InvalidRequest);

    for (const auto& e : engine.emittedEvents()) {
        assert(!std::holds_alternative<Fill>(e));
        assert(!std::holds_alternative<AccountingEvent>(e));
        assert(!std::holds_alternative<AccountSnapshot>(e));
    }

    return 0;
}
