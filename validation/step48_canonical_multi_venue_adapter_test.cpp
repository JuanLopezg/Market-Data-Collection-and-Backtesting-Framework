#include <cassert>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "canonical_venue_adapter.h"

using namespace VenueContracts::V1;

namespace {

InstrumentIdentity btcPerp()
{
    InstrumentIdentity result;
    result.market.canonical_asset = "BTC";
    result.market.product_class = ProductClass::Perpetual;
    result.market.quote_or_settlement_asset = "USD";
    result.venue_symbol = "BTC-PERP-EXPLICIT";
    result.venue_asset_id = "opaque-native-asset";
    return result;
}

RequestIdentity request(const std::string& id)
{
    RequestIdentity result;
    result.venue = VenueContext{"MOCK", VenueEnvironment::Mock, {}};
    result.request_id = id;
    result.correlation_id = "corr-" + id;
    result.requested_at = 100;
    return result;
}

class ProbeAdapter final : public CanonicalVenueAdapter {
public:
    VenueContext ctx{"FUTURE_VENUE", VenueEnvironment::Custom, "SANDBOX"};
    CapabilitySet caps{
        Capability::MarketMetadata,
        Capability::TradingRules,
        Capability::SubmitOrder,
        Capability::CancelOrder,
        Capability::AccountSnapshot,
        Capability::OpenOrders,
        Capability::Fills,
        Capability::SnapshotBackfill,
        Capability::FeeAccounting
    };

    EventHandler handler;
    int submits = 0;
    int polls = 0;

    VenueContext context() const override { return ctx; }
    CapabilitySet capabilities() const override { return caps; }

    void setEventHandler(EventHandler value) override { handler = std::move(value); }

    void submitOrders(const SubmitOrderBatch&) override { ++submits; }
    void cancelOrders(const CancelOrderBatch&) override {}
    void modifyOrders(const ModifyOrderBatch&) override {}
    void requestAccountSnapshot(const AccountSnapshotRequest&) override {}
    void requestOpenOrders(const OpenOrdersRequest&) override {}
    void requestOrderStatus(const OrderStatusRequest&) override {}
    void requestFills(const FillBackfillRequest&) override {}

    void poll(int timeout_ms) override
    {
        assert(timeout_ms > 0);
        ++polls;
    }
};

} // namespace

int main()
{
    static_assert(std::has_virtual_destructor<CanonicalVenueAdapter>::value,
                  "CanonicalVenueAdapter must be safely polymorphic");

    VenueContext mock{"MOCK", VenueEnvironment::Mock, {}};
    assert(mock.valid());

    MarketIdentity market{"BTC", ProductClass::Perpetual, "USD", {}};
    assert(market.valid());
    assert(std::string(toString(market.product_class)) == "PERPETUAL");

    InstrumentIdentity instrument = btcPerp();
    assert(instrument.valid());

    CapabilitySet resolved{
        Capability::OrderStatusQuery,
        Capability::TriggerOrders,
        Capability::FeeAccounting,
        Capability::FundingAccounting
    };
    assert(resolved.supportsAll({
        Capability::OrderStatusQuery,
        Capability::FeeAccounting,
        Capability::FundingAccounting
    }));

    CapabilitySet futureVenue{
        Capability::SubmitOrder,
        Capability::CancelOrder,
        Capability::AccountSnapshot,
        Capability::OpenOrders,
        Capability::Fills
    };
    assert(!futureVenue.supports(Capability::ModifyOrder));
    assert(!futureVenue.supports(Capability::ClientOrderId));
    assert(!futureVenue.supports(Capability::PostOnly));
    assert(futureVenue.missing({
        Capability::SubmitOrder,
        Capability::ModifyOrder,
        Capability::PostOnly
    }).size() == 2);

    LimitOrderIntent order;
    order.local_order_id = 42;
    order.strategy_id = 1;
    order.created_at = 100;
    order.active_from = 101;
    order.instrument = instrument;
    order.side = Side::Buy;
    order.quantity = 0.01;
    order.limit_price = 50000.0;
    order.time_in_force = TimeInForce::Gtc;
    order.client_order_id = "correlation-only";
    assert(order.valid());

    SubmitOrderBatch submit;
    submit.request = request("req-1");
    submit.items.push_back(SubmitOrderItem{"item-1", order});
    assert(submit.valid());
    assert(submit.request.request_id != submit.items[0].order.client_order_id);

    Error noError;
    noError.classification = ErrorClass::None;

    ItemResult accepted;
    accepted.item_id = "item-1";
    accepted.accepted = true;
    accepted.error = noError;
    accepted.native_references.native_order_id = "native-1";
    assert(accepted.valid());

    OperationResult itemResult;
    itemResult.request = submit.request;
    itemResult.command = CommandKind::Submit;
    itemResult.scope = ResultScope::Item;
    itemResult.accepted = true;
    itemResult.operation_error = noError;
    itemResult.item_results.push_back(accepted);
    assert(itemResult.valid());

    OperationResult wholeOperationRejected;
    wholeOperationRejected.request = request("req-2");
    wholeOperationRejected.command = CommandKind::Submit;
    wholeOperationRejected.scope = ResultScope::Operation;
    wholeOperationRejected.accepted = false;
    wholeOperationRejected.operation_error.classification = ErrorClass::InvalidRequest;
    assert(wholeOperationRejected.valid());
    assert(wholeOperationRejected.item_results.empty());

    assert(isTerminal(OrderLifecycleStatus::Filled));
    assert(isTerminal(OrderLifecycleStatus::VenueTerminated));
    assert(!isTerminal(OrderLifecycleStatus::UnknownRequiresReconciliation));
    assert(requiresReconciliation(OrderLifecycleStatus::UnknownRequiresReconciliation));

    Fill fill;
    fill.venue = mock;
    fill.instrument = instrument;
    fill.local_order_id = 42;
    fill.strategy_id = 1;
    fill.timestamp = 102;
    fill.side = Side::Buy;
    fill.quantity = 0.01;
    fill.price = 49999.0;
    fill.native_references.native_fill_id = "opaque-stable-fill-ref";
    assert(fill.valid());

    assert(isDecimalString("-1.25"));
    assert(isDecimalString("0"));
    assert(!isDecimalString("1e-3"));
    assert(!isDecimalString(".5"));

    AccountingEvent fee;
    fee.venue = mock;
    fee.timestamp = 102;
    fee.type = AccountingEventType::TradingFee;
    fee.amount = "-0.25";
    fee.settlement_asset = "USDC";
    fee.market = market;
    assert(fee.valid());
    assert(std::string(toString(fee.type)) == "TRADING_FEE");

    Event event = fill;
    assert(std::holds_alternative<Fill>(event));

    ProbeAdapter adapter;
    assert(adapter.context().valid());
    assert(adapter.capabilities().supports(Capability::SubmitOrder));
    assert(!adapter.capabilities().supports(Capability::ModifyOrder));

    const auto missing = adapter.capabilities().missing({
        Capability::SubmitOrder,
        Capability::ModifyOrder
    });
    assert(missing.size() == 1);
    assert(missing.front() == Capability::ModifyOrder);

    adapter.submitOrders(submit);
    adapter.poll(1);
    assert(adapter.submits == 1);
    assert(adapter.polls == 1);

    return 0;
}
