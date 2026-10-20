#include <gtest/gtest.h>

#include <string>
#include <variant>
#include <vector>

#include "canonical_venue_adapter.h"
#include "mock/exchange.h"
#include "test_support.h"

namespace {
namespace M = MockVenue;
namespace V = VenueContracts::V1;

M::MatchingFillConfig matchingConfig()
{
    M::MatchingFillConfig config;
    config.max_adverse_slippage_ppm = 0;
    return config;
}

V::RequestIdentity request(const std::string& id, Timestamp at = 10)
{
    return {M::context(), id, "canonical-correlation", at};
}

V::LimitOrderIntent intent(OrderID id = 1)
{
    const auto* btc = M::findByCanonicalAssetExact("BTCUSDT");
    if (!btc) throw std::runtime_error("Synthetic BTCUSDT catalog entry is required");
    V::LimitOrderIntent result;
    result.local_order_id = id;
    result.strategy_id = 7;
    result.created_at = 10;
    result.active_from = 20;
    result.instrument = btc->instrument();
    result.side = V::Side::Buy;
    result.quantity = 0.2;
    result.limit_price = 100.0;
    result.client_order_id = "canonical-client-" + std::to_string(id);
    return result;
}

V::SubmitOrderBatch submission(const std::string& id = "submit-1", OrderID local = 1,
                               Timestamp at = 10)
{
    return {request(id, at), {{"item-" + id, intent(local)}}};
}

M::MarketBarObservation observation(Timestamp at)
{
    return {"BTCUSDT", at, {100.0, 101.0, 99.0, 100.0, 1.0}};
}

template <class T>
std::vector<T> select(const std::vector<V::Event>& events)
{
    std::vector<T> result;
    for (const auto& event : events)
        if (const auto* value = std::get_if<T>(&event)) result.push_back(*value);
    return result;
}

struct VenueFixture {
    test_support::TempDir directory{"adapters-canonical"};
    M::MockExchange venue{directory.path(), {}, matchingConfig()};
    std::vector<V::Event> events;
    VenueFixture() { venue.setEventHandler([this](const V::Event& event) { events.push_back(event); }); }
};

TEST(CanonicalAdapter, R12_ExplicitContextCapabilitiesAndAdmittedOrderNeverImplyFill)
{
    VenueFixture h;
    CanonicalVenueAdapter& adapter = h.venue;
    EXPECT_EQ(adapter.context().venue_id, "MOCK");
    EXPECT_EQ(adapter.context().environment, VenueContracts::VenueEnvironment::Mock);
    EXPECT_TRUE(adapter.capabilities().supportsAll({V::Capability::SubmitOrder,
        V::Capability::CancelOrder, V::Capability::AccountSnapshot, V::Capability::OrderStatusQuery}));
    EXPECT_FALSE(adapter.capabilities().supports(V::Capability::DeadMansSwitch));
    EXPECT_EQ(adapter.capabilities().missing({V::Capability::SubmitOrder, V::Capability::DeadMansSwitch}),
              std::vector<V::Capability>{V::Capability::DeadMansSwitch});
    ASSERT_TRUE(h.venue.reconcile(9).clean());
    adapter.submitOrders(submission());
    const auto results = select<V::OperationResult>(h.events);
    ASSERT_EQ(results.size(), 1U);
    ASSERT_EQ(results.front().item_results.size(), 1U);
    EXPECT_TRUE(results.front().item_results.front().accepted);
    EXPECT_TRUE(select<V::Fill>(h.events).empty());
}

TEST(CanonicalAdapter, R12_MismatchedNativeIdentityRejectsWithoutFallbackOrFill)
{
    VenueFixture h;
    CanonicalVenueAdapter& adapter = h.venue;
    ASSERT_TRUE(h.venue.reconcile(9).clean());
    auto batch = submission();
    batch.items.front().order.instrument.venue_asset_id = "foreign-market-id";
    adapter.submitOrders(batch);
    const auto results = select<V::OperationResult>(h.events);
    ASSERT_EQ(results.size(), 1U);
    ASSERT_EQ(results.front().item_results.size(), 1U);
    EXPECT_FALSE(results.front().item_results.front().accepted);
    EXPECT_TRUE(select<V::Fill>(h.events).empty());
    EXPECT_TRUE(h.venue.chaos().runtime().lifecycle().orders().empty());
    EXPECT_EQ(adapter.context().venue_id, "MOCK");
}

TEST(CanonicalAdapter, R13_OrderedStreamPublishesFillBeforeItsFeeAndPartialStatus)
{
    VenueFixture h;
    ASSERT_TRUE(h.venue.reconcile(9).clean());
    CanonicalVenueAdapter& adapter = h.venue;
    adapter.submitOrders(submission());
    ASSERT_EQ(h.events.size(), 2U);
    EXPECT_TRUE(std::holds_alternative<V::OperationResult>(h.events[0]));
    EXPECT_TRUE(std::holds_alternative<V::OrderUpdate>(h.events[1]));
    h.events.clear();
    h.venue.processMarketBar(observation(20));
    ASSERT_GE(h.events.size(), 3U);
    ASSERT_TRUE(std::holds_alternative<V::Fill>(h.events[0]));
    ASSERT_TRUE(std::holds_alternative<V::AccountingEvent>(h.events[1]));
    ASSERT_TRUE(std::holds_alternative<V::OrderUpdate>(h.events[2]));
    const auto& execution = std::get<V::Fill>(h.events[0]);
    EXPECT_DOUBLE_EQ(execution.quantity, 0.1);
    EXPECT_DOUBLE_EQ(execution.price, 100.0);
    const auto& fee = std::get<V::AccountingEvent>(h.events[1]);
    EXPECT_EQ(fee.amount, "-0.00400000");
    EXPECT_EQ(fee.correlation_id, "fee:" + execution.native_references.native_fill_id);
    EXPECT_EQ(std::get<V::OrderUpdate>(h.events[2]).status, V::OrderLifecycleStatus::PartiallyFilled);
    const auto updates = select<V::OrderUpdate>(h.events);
    for (const auto& update : updates) {
        EXPECT_EQ(update.local_order_id, 1U);
        EXPECT_EQ(update.status, V::OrderLifecycleStatus::PartiallyFilled);
        EXPECT_DOUBLE_EQ(update.cumulative_filled_quantity, 0.1);
    }
    EXPECT_EQ(updates.size(), 1U);
    EXPECT_EQ(h.events.size(), 3U);
}

TEST(CanonicalAdapter, R13_SnapshotsExposePerpetualCashAndPartialOrderWithoutMutation)
{
    VenueFixture h;
    ASSERT_TRUE(h.venue.reconcile(9).clean());
    CanonicalVenueAdapter& adapter = h.venue;
    adapter.submitOrders(submission());
    h.venue.processMarketBar(observation(20));
    const auto before = h.venue.chaos().runtime().stateSnapshot(20);
    h.events.clear();
    adapter.requestAccountSnapshot({request("account", 21)});
    adapter.requestOpenOrders({request("open-orders", 21)});
    const auto accounts = select<V::AccountSnapshot>(h.events);
    const auto open = select<V::OpenOrdersSnapshot>(h.events);
    ASSERT_EQ(accounts.size(), 1U);
    ASSERT_EQ(accounts.front().balances.size(), 1U);
    ASSERT_EQ(accounts.front().positions.size(), 1U);
    EXPECT_EQ(accounts.front().balances.front().asset, "USD");
    EXPECT_EQ(accounts.front().positions.front().instrument.market.canonical_asset, "BTCUSDT");
    EXPECT_NEAR(accounts.front().balances.front().total, 99999.996, 1e-8);
    EXPECT_DOUBLE_EQ(accounts.front().positions.front().signed_quantity, 0.1);
    ASSERT_EQ(open.size(), 1U);
    ASSERT_EQ(open.front().orders.size(), 1U);
    EXPECT_DOUBLE_EQ(open.front().orders.front().quantity, 0.2);
    EXPECT_DOUBLE_EQ(open.front().orders.front().cumulative_filled_quantity, 0.1);
    const auto after = h.venue.chaos().runtime().stateSnapshot(21);
    EXPECT_EQ(after.snapshot_sequence, before.snapshot_sequence);
    EXPECT_EQ(after.economic_fingerprint, before.economic_fingerprint);
    EXPECT_EQ(after.stream_fingerprint, before.stream_fingerprint);
}

TEST(CanonicalAdapter, R13_OrderStatusDistinguishesKnownPartialAndUnknownReconciliation)
{
    VenueFixture h;
    ASSERT_TRUE(h.venue.reconcile(9).clean());
    CanonicalVenueAdapter& adapter = h.venue;
    adapter.submitOrders(submission());
    h.venue.processMarketBar(observation(20));
    h.events.clear();
    V::OrderLocator known{1, intent().instrument, {}};
    V::OrderLocator unknown{99, intent().instrument, {}};
    adapter.requestOrderStatus({request("known", 21), known});
    adapter.requestOrderStatus({request("unknown", 21), unknown});
    const auto statuses = select<V::OrderUpdate>(h.events);
    ASSERT_EQ(statuses.size(), 2U);
    EXPECT_EQ(statuses[0].status, V::OrderLifecycleStatus::PartiallyFilled);
    EXPECT_DOUBLE_EQ(statuses[0].cumulative_filled_quantity, 0.1);
    EXPECT_EQ(statuses[1].local_order_id, 99U);
    EXPECT_EQ(statuses[1].status, V::OrderLifecycleStatus::UnknownRequiresReconciliation);
    EXPECT_EQ(statuses[1].terminal_reason.classification, V::ErrorClass::OrderNotFound);
    EXPECT_TRUE(select<V::Fill>(h.events).empty());
}

TEST(CanonicalAdapter, R13_FillBackfillUsesInclusiveBoundsAndStableExecutionIdentity)
{
    VenueFixture h;
    ASSERT_TRUE(h.venue.reconcile(9).clean());
    CanonicalVenueAdapter& adapter = h.venue;
    adapter.submitOrders(submission());
    h.venue.processMarketBar(observation(20));
    h.venue.processMarketBar(observation(30));
    const auto executions = select<V::Fill>(h.events);
    ASSERT_EQ(executions.size(), 2U);
    h.events.clear();
    adapter.requestFills({request("backfill", 31), 20, 20, {}});
    const auto batches = select<V::FillBatch>(h.events);
    ASSERT_EQ(batches.size(), 1U);
    ASSERT_EQ(batches.front().fills.size(), 1U);
    const auto& execution = batches.front().fills.front();
    EXPECT_EQ(execution.timestamp, 20U);
    EXPECT_EQ(execution.native_references.native_fill_id, executions[0].native_references.native_fill_id);
    EXPECT_DOUBLE_EQ(execution.quantity, 0.1);
    EXPECT_DOUBLE_EQ(execution.price, 100.0);
}

TEST(CanonicalAdapter, R14_DelayedReceiptAndRetryCannotCreateSecondAcceptedOrder)
{
    VenueFixture h;
    ASSERT_TRUE(h.venue.reconcile(9).clean());
    CanonicalVenueAdapter& adapter = h.venue;
    h.venue.chaos().injectNext(M::ChaosFault::DelayResponse);
    const auto batch = submission();
    adapter.submitOrders(batch);
    EXPECT_TRUE(select<V::OperationResult>(h.events).empty());
    EXPECT_EQ(select<V::OrderUpdate>(h.events).size(), 1U);
    adapter.poll(0); // Explicit technical release, independent of business dates.
    EXPECT_EQ(select<V::OperationResult>(h.events).size(), 1U);
    ASSERT_TRUE(h.venue.reconcile(10).clean());
    adapter.submitOrders(batch);
    EXPECT_EQ(select<V::OperationResult>(h.events).size(), 2U);
    EXPECT_EQ(select<V::OrderUpdate>(h.events).size(), 1U);
    EXPECT_EQ(h.venue.chaos().runtime().lifecycle().orders().size(), 1U);
    EXPECT_TRUE(select<V::Fill>(h.events).empty());
}

TEST(CanonicalAdapter, R14_CancelAndModifyPreserveIdentityWithoutInventingExecution)
{
    VenueFixture h;
    ASSERT_TRUE(h.venue.reconcile(9).clean());
    CanonicalVenueAdapter& adapter = h.venue;
    adapter.submitOrders(submission());
    auto replacement = intent();
    replacement.quantity = 0.3;
    V::OrderLocator locator{1, intent().instrument, {}};
    adapter.modifyOrders({request("modify", 11), {{"modify-item", locator, replacement}}});
    adapter.cancelOrders({request("cancel", 12), {{"cancel-item", locator}}});
    const auto statuses = select<V::OrderUpdate>(h.events);
    ASSERT_GE(statuses.size(), 3U);
    EXPECT_EQ(statuses.back().status, V::OrderLifecycleStatus::Canceled);
    for (const auto& update : statuses) {
        EXPECT_EQ(update.local_order_id, 1U);
        EXPECT_EQ(update.native_references.native_order_id, "mock-order-1");
        EXPECT_DOUBLE_EQ(update.cumulative_filled_quantity, 0.0);
    }
    h.venue.processMarketBar(observation(20));
    EXPECT_TRUE(select<V::Fill>(h.events).empty());
}

TEST(CanonicalAdapter, R14_AmbiguousSubmitRequiresReconcileBeforeIdempotentRetry)
{
    VenueFixture h;
    ASSERT_TRUE(h.venue.reconcile(9).clean());
    CanonicalVenueAdapter& adapter = h.venue;
    h.venue.chaos().injectNext(M::ChaosFault::LoseResponseAmbiguousSubmit);
    const auto batch = submission();
    adapter.submitOrders(batch);
    EXPECT_TRUE(select<V::OperationResult>(h.events).empty());
    EXPECT_EQ(select<V::OrderUpdate>(h.events).size(), 1U);
    EXPECT_FALSE(h.venue.canRouteNewSubmit());
    adapter.submitOrders(submission("new-while-ambiguous", 2, 11));
    EXPECT_EQ(h.venue.chaos().runtime().lifecycle().findOrder(2), nullptr);
    ASSERT_TRUE(h.venue.reconcile(12).clean());
    adapter.submitOrders(batch);
    EXPECT_EQ(h.venue.chaos().runtime().lifecycle().orders().size(), 1U);
    EXPECT_EQ(select<V::OrderUpdate>(h.events).size(), 1U);
    EXPECT_TRUE(select<V::Fill>(h.events).empty());
}

TEST(CanonicalAdapter, R14_RestartExposesRecoveredTruthWithoutReemittingHistoricalFills)
{
    test_support::TempDir directory("adapters-canonical-restart");
    M::UserStateSnapshot saved;
    {
        M::MockExchange first(directory.path(), {}, matchingConfig());
        ASSERT_TRUE(first.reconcile(9).clean());
        CanonicalVenueAdapter& adapter = first;
        adapter.submitOrders(submission());
        first.processMarketBar(observation(20));
        saved = first.chaos().runtime().stateSnapshot(20);
        ASSERT_TRUE(first.chaos().runtime().checkpoint());
    }
    M::MockExchange restored(directory.path(), {}, matchingConfig());
    CanonicalVenueAdapter& adapter = restored;
    std::vector<V::Event> events;
    adapter.setEventHandler([&](const V::Event& event) { events.push_back(event); });
    EXPECT_TRUE(events.empty());
    const auto recovered = restored.chaos().runtime().stateSnapshot(21);
    EXPECT_EQ(recovered.economic_fingerprint, saved.economic_fingerprint);
    EXPECT_EQ(recovered.stream_fingerprint, saved.stream_fingerprint);
    adapter.requestAccountSnapshot({request("after-restart", 21)});
    const auto accounts = select<V::AccountSnapshot>(events);
    ASSERT_EQ(accounts.size(), 1U);
    EXPECT_NEAR(accounts.front().balances.front().total, 99999.996, 1e-8);
    EXPECT_DOUBLE_EQ(accounts.front().positions.front().signed_quantity, 0.1);
    EXPECT_TRUE(select<V::Fill>(events).empty());
    restored.processMarketBar(observation(30));
    const auto executions = select<V::Fill>(events);
    ASSERT_EQ(executions.size(), 1U);
    EXPECT_EQ(executions.front().timestamp, 30U);
    EXPECT_DOUBLE_EQ(executions.front().quantity, 0.1);
}

} // namespace
