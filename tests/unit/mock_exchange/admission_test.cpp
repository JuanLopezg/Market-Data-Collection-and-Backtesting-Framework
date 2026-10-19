#include <gtest/gtest.h>

#include <limits>
#include <vector>

#include "mock_test_support.h"

namespace {
using namespace mock_test;

TEST(MockAdmission, R1_CatalogIdentityAndEconomicRulesRequireExactLookup)
{
    const auto* btc = M::findByCanonicalAssetExact("BTCUSDT");
    ASSERT_NE(btc, nullptr);
    EXPECT_EQ(M::findByVenueSymbolExact(btc->venue_symbol), btc);
    EXPECT_EQ(M::findByCanonicalAssetExact("btcusdt"), nullptr);
    EXPECT_EQ(M::findByCanonicalAssetExact("BTC/USD"), nullptr);
    EXPECT_EQ(M::findByCanonicalAssetExact("UNKNOWN"), nullptr);
    EXPECT_EQ(M::catalog().size(), 175U);
    const auto* rules = M::rulesForExact(*btc);
    ASSERT_NE(rules, nullptr);
    EXPECT_EQ(rules->price_increment, "0.00000001");
    EXPECT_EQ(rules->size_increment, "0.00000001");
    EXPECT_EQ(rules->min_notional, "10.00");
    M::MockOrders life;
    life.submit(submit(order(1, V::Side::Buy, 0.1)));
    ASSERT_NE(life.findOrder(1), nullptr);
    EXPECT_DOUBLE_EQ(life.findOrder(1)->remaining_quantity, 0.1);
}

TEST(MockAdmission, R2_InstrumentOrVenueMismatchCannotCreateAcceptedOrders)
{
    for (int invalid = 0; invalid < 6; ++invalid) {
        SCOPED_TRACE(invalid);
        M::MockOrders life;
        auto batch = submit();
        auto& instrument = batch.items.front().order.instrument;
        if (invalid == 0) instrument.market.canonical_asset = "btcusdt";
        if (invalid == 1) instrument.venue_symbol = "BTC/USD";
        if (invalid == 2) instrument.venue_asset_id = "wrong-native-id";
        if (invalid == 3) instrument.market.quote_or_settlement_asset = "USDT";
        if (invalid == 4) instrument.market.product_class = V::ProductClass::Spot;
        if (invalid == 5) batch.request.venue.venue_id = "OTHER";
        life.submit(batch);
        EXPECT_EQ(life.findOrder(1), nullptr);
        const auto results = eventsOf<V::OperationResult>(life.emittedEvents());
        ASSERT_EQ(results.size(), 1U);
        if (results.front().scope == V::ResultScope::Operation) {
            EXPECT_FALSE(results.front().accepted);
        } else {
            ASSERT_EQ(results.front().item_results.size(), 1U);
            EXPECT_FALSE(results.front().item_results.front().accepted);
        }
        EXPECT_TRUE(eventsOf<V::Fill>(life.emittedEvents()).empty());
    }
}

TEST(MockAdmission, R2_PriceAndSizeNinthDecimalsAreRejectedWithoutRounding)
{
    for (const bool invalidPrice : {false, true}) {
        M::MockOrders life;
        auto intent = order();
        if (invalidPrice) intent.limit_price = 100.000000001;
        else intent.quantity = 0.200000001;
        life.submit(submit(intent));
        const auto results = eventsOf<V::OperationResult>(life.emittedEvents());
        ASSERT_EQ(results.size(), 1U);
        ASSERT_EQ(results.front().item_results.size(), 1U);
        const auto& result = results.front().item_results.front();
        EXPECT_FALSE(result.accepted);
        EXPECT_EQ(result.error.classification, invalidPrice ? V::ErrorClass::InvalidPriceIncrement
                                                           : V::ErrorClass::InvalidQuantityIncrement);
        EXPECT_EQ(life.findOrder(1), nullptr);
    }
}

TEST(MockAdmission, R1_MinimumNotionalIncludesExactTenDollarsAndRejectsBelow)
{
    for (const double quantity : {0.1, 0.09999999}) {
        M::MockOrders life;
        life.submit(submit(order(1, V::Side::Buy, quantity)));
        const auto results = eventsOf<V::OperationResult>(life.emittedEvents());
        ASSERT_EQ(results.size(), 1U);
        ASSERT_EQ(results.front().item_results.size(), 1U);
        EXPECT_EQ(results.front().item_results.front().accepted, quantity == 0.1);
        if (quantity != 0.1) {
            EXPECT_EQ(results.front().item_results.front().error.classification, V::ErrorClass::BelowMinimum);
            EXPECT_EQ(life.findOrder(1), nullptr);
        }
    }
}

TEST(MockAdmission, R2_InvalidNumbersAndPostOnlyIocCannotBeAdmitted)
{
    for (int invalid = 0; invalid < 7; ++invalid) {
        M::MockOrders life;
        auto intent = order();
        if (invalid == 0) intent.quantity = 0.0;
        if (invalid == 1) intent.quantity = -1.0;
        if (invalid == 2) intent.quantity = std::numeric_limits<double>::quiet_NaN();
        if (invalid == 3) intent.limit_price = 0.0;
        if (invalid == 4) intent.limit_price = std::numeric_limits<double>::infinity();
        if (invalid == 5) { intent.post_only = true; intent.time_in_force = V::TimeInForce::Ioc; }
        if (invalid == 6) intent.quantity = 0.000000001;
        life.submit(submit(intent));
        EXPECT_EQ(life.findOrder(1), nullptr);
        EXPECT_TRUE(eventsOf<V::Fill>(life.emittedEvents()).empty());
        const auto results = eventsOf<V::OperationResult>(life.emittedEvents());
        ASSERT_EQ(results.size(), 1U);
        ASSERT_EQ(results.front().item_results.size(), 1U);
        EXPECT_FALSE(results.front().item_results.front().accepted);
    }
}

TEST(MockAdmission, R3_HistoricalLocalAndClientIdsRemainReservedAfterCancel)
{
    M::MockOrders life;
    life.submit(submit());
    life.cancel(cancel());
    life.clearEmittedEvents();
    life.submit(submit(order(), "reuse-local", 31));
    auto other = order(2);
    other.client_order_id = order().client_order_id;
    life.submit(submit(other, "reuse-client", 32));
    EXPECT_EQ(life.orders().size(), 1U);
    EXPECT_EQ(life.findOrder(1)->status, V::OrderLifecycleStatus::Canceled);
    const auto results = eventsOf<V::OperationResult>(life.emittedEvents());
    ASSERT_EQ(results.size(), 2U);
    for (const auto& result : results) {
        ASSERT_EQ(result.item_results.size(), 1U);
        EXPECT_FALSE(result.item_results.front().accepted);
        EXPECT_EQ(result.item_results.front().error.classification, V::ErrorClass::DuplicateRequest);
    }
}

TEST(MockAdmission, R3_IdenticalRequestRetriesReturnResultWithoutNewLifecycleOrQuantity)
{
    M::MockOrders life;
    const auto batch = submit();
    life.submit(batch);
    ASSERT_GE(life.emittedEvents().size(), 2U);
    EXPECT_TRUE(std::holds_alternative<V::OperationResult>(life.emittedEvents()[0]));
    EXPECT_TRUE(std::holds_alternative<V::OrderUpdate>(life.emittedEvents()[1]));
    life.clearEmittedEvents();
    for (int retry = 0; retry < 10; ++retry) life.submit(batch);
    EXPECT_EQ(eventsOf<V::OperationResult>(life.emittedEvents()).size(), 10U);
    EXPECT_TRUE(eventsOf<V::OrderUpdate>(life.emittedEvents()).empty());
    EXPECT_EQ(life.orders().size(), 1U);
    EXPECT_DOUBLE_EQ(life.findOrder(1)->remaining_quantity, 0.2);
}

TEST(MockAdmission, R3_RequestIdConflictRejectsChangedPayloadAtOperationScope)
{
    M::MockOrders life;
    auto batch = submit();
    life.submit(batch);
    life.clearEmittedEvents();
    batch.items.front().order.quantity = 0.3;
    life.submit(batch);
    const auto results = eventsOf<V::OperationResult>(life.emittedEvents());
    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results.front().scope, V::ResultScope::Operation);
    EXPECT_FALSE(results.front().accepted);
    EXPECT_EQ(results.front().operation_error.classification, V::ErrorClass::DuplicateRequest);
    EXPECT_DOUBLE_EQ(life.findOrder(1)->intent.quantity, 0.2);
}

TEST(MockAdmission, R2_MixedBatchAdmitsOnlyValidEconomicItems)
{
    M::MockOrders life;
    auto batch = submit();
    batch.items.push_back({"below-minimum", order(2, V::Side::Buy, 0.01)});
    life.submit(batch);
    const auto results = eventsOf<V::OperationResult>(life.emittedEvents());
    ASSERT_EQ(results.size(), 1U);
    EXPECT_TRUE(results.front().accepted);
    EXPECT_EQ(results.front().scope, V::ResultScope::Item);
    ASSERT_EQ(results.front().item_results.size(), 2U);
    EXPECT_TRUE(results.front().item_results[0].accepted);
    EXPECT_FALSE(results.front().item_results[1].accepted);
    EXPECT_NE(life.findOrder(1), nullptr);
    EXPECT_EQ(life.findOrder(2), nullptr);
}

TEST(MockAdmission, R4_CancelLifecyclePreventsFutureRemainderFills)
{
    M::MockOrders life;
    life.submit(submit());
    life.clearEmittedEvents();
    life.cancel(cancel());
    const auto updates = eventsOf<V::OrderUpdate>(life.emittedEvents());
    ASSERT_EQ(updates.size(), 2U);
    EXPECT_EQ(updates[0].status, V::OrderLifecycleStatus::CancelPending);
    EXPECT_EQ(updates[1].status, V::OrderLifecycleStatus::Canceled);
    M::MockMatching matcher(life, matching());
    matcher.processBar(bar(40, 100.0));
    EXPECT_TRUE(eventsOf<V::Fill>(matcher.emittedEvents()).empty());
}

TEST(MockAdmission, R4_ModifyPreservesFilledUnitsAndRevalidatesIdentityAndGrid)
{
    M::MockOrders life;
    const auto original = order();
    life.submit(submit(original));
    M::MockMatching matcher(life, matching());
    matcher.processBar(bar());
    auto replacement = original;
    replacement.quantity = 0.3;
    life.modify(modify(original, replacement));
    ASSERT_NE(life.findOrder(1), nullptr);
    EXPECT_DOUBLE_EQ(life.findOrder(1)->cumulative_filled_quantity, 0.1);
    EXPECT_NEAR(life.findOrder(1)->remaining_quantity, 0.2, 1e-12);
    EXPECT_EQ(life.findOrder(1)->native_references.native_order_id, "mock-order-1");
    for (const bool wrongIdentity : {false, true}) {
        auto bad = replacement;
        if (wrongIdentity) bad.instrument = order(2, V::Side::Buy, 0.2, 100.0, "ETHUSDT").instrument;
        else bad.limit_price = 100.000000001;
        life.clearEmittedEvents();
        life.modify(modify(original, bad, wrongIdentity ? "bad-identity" : "bad-grid"));
        const auto results = eventsOf<V::OperationResult>(life.emittedEvents());
        ASSERT_EQ(results.size(), 1U);
        EXPECT_FALSE(results.front().item_results.front().accepted);
        EXPECT_DOUBLE_EQ(life.findOrder(1)->intent.quantity, 0.3);
    }
}

TEST(MockAdmission, DISABLED_R4_ModifyCannotShrinkBelowAlreadyExecutedQuantity)
{
    // FINDING: F1 - Modify accepts quantity 0.05 after 0.1 has filled, rewriting
    // the original 0.2 request; R4 requires rejection without changing progress.
    M::MockOrders life;
    const auto original = order();
    life.submit(submit(original));
    M::MockMatching matcher(life, matching());
    matcher.processBar(bar()); // Actual 0.1 fill.
    auto replacement = original;
    replacement.quantity = 0.05;
    replacement.limit_price = 200.0; // Still USD 10, so admission size is otherwise valid.
    life.clearEmittedEvents();
    life.modify(modify(original, replacement));
    const auto results = eventsOf<V::OperationResult>(life.emittedEvents());
    ASSERT_EQ(results.size(), 1U);
    ASSERT_EQ(results.front().item_results.size(), 1U);
    EXPECT_FALSE(results.front().item_results.front().accepted);
    EXPECT_DOUBLE_EQ(life.findOrder(1)->intent.quantity, 0.2);
    EXPECT_DOUBLE_EQ(life.findOrder(1)->cumulative_filled_quantity, 0.1);
    EXPECT_DOUBLE_EQ(life.findOrder(1)->remaining_quantity, 0.1);
}

} // namespace
