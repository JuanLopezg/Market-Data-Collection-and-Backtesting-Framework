// Venue journal: durable orders, fills and cash entries (tests/specs/exchange_gateway.md, Part J).

#include <memory>
#include <stdexcept>

#include <gtest/gtest.h>

#include "test_support.h"
#include "venue_fakes.h"

using namespace venue_fakes;

namespace {

JournalOrder openOrder(OrderID id)
{
    JournalOrder order;
    order.local_order_id = id;
    order.strategy_id = 1;
    order.client_order_id = "algo-t1-" + std::to_string(id);
    order.coin = "BTCUSDT";
    order.symbol = "PF_XBTUSD";
    order.side = OrderSide::Buy;
    order.source_quantity = 0.1;
    order.venue_quantity = 0.1;
    order.unit_scale = 1.0;
    order.active_from = TODAY;
    order.state = OrderState::Open;
    order.venue_order_id = "v-" + std::to_string(id);
    order.sent_at_ms = T0_MS;
    order.updated_at_ms = T0_MS;
    return order;
}

VenueFill venueFill(const std::string& id, OrderID order, double quantity)
{
    VenueFill fill;
    fill.venue_fill_id = id;
    fill.venue_order_id = "v-" + std::to_string(order);
    fill.client_order_id = "algo-t1-" + std::to_string(order);
    fill.symbol = "PF_XBTUSD";
    fill.side = OrderSide::Buy;
    fill.quantity = quantity;
    fill.price = 62000.0;
    fill.day = TODAY;
    fill.fill_time_ms = T0_MS + 2000;
    fill.fill_time = "2026-10-12T00:00:07.000Z";
    fill.fill_type = "taker";
    return fill;
}

} // namespace


TEST(VenueJournal, J1_BoundToOneVenueAndNamespace)
{
    test_support::TempDir dir("venue-journal");
    const std::string path = (dir / "kraken.sqlite").string();
    { VenueJournal journal(path, "kraken-futures", "t1"); }
    EXPECT_THROW(VenueJournal(path, "kraken-futures", "t2"), std::runtime_error);
    EXPECT_THROW(VenueJournal(path, "other-venue", "t1"), std::runtime_error);
    EXPECT_NO_THROW(VenueJournal(path, "kraken-futures", "t1"));
}

TEST(VenueJournal, J2_OrdersAndFillsSurviveReopen)
{
    test_support::TempDir dir("venue-journal");
    const std::string path = (dir / "kraken.sqlite").string();
    {
        VenueJournal journal(path, "kraken-futures", "t1");
        journal.insertOrder(openOrder(7));
        journal.insertOrder(openOrder(8));
        JournalOrder updated = openOrder(7);
        updated.filled_venue_quantity = 0.04;
        EXPECT_EQ(journal.recordFill(venueFill("F1", 7, 0.04), updated).local_fill_id, 1u);
        updated.filled_venue_quantity = 0.1;
        updated.state = OrderState::Filled;
        EXPECT_EQ(journal.recordFill(venueFill("F2", 7, 0.06), updated).local_fill_id, 2u);
        journal.markFillPublished("F1");
    }
    VenueJournal reopened(path, "kraken-futures", "t1");
    const auto order = reopened.order(7);
    ASSERT_TRUE(order.has_value());
    EXPECT_EQ(order->state, OrderState::Filled);
    EXPECT_DOUBLE_EQ(order->filled_venue_quantity, 0.1);
    EXPECT_EQ(order->client_order_id, "algo-t1-7");
    EXPECT_EQ(reopened.orderByClientId("algo-t1-8")->local_order_id, 8u);
    EXPECT_EQ(reopened.orderByVenueId("v-8")->local_order_id, 8u);
    EXPECT_TRUE(reopened.knowsFill("F1"));
    EXPECT_TRUE(reopened.knowsFill("F2"));
    EXPECT_FALSE(reopened.knowsFill("F3"));

    const auto unpublished = reopened.unpublishedFills();
    ASSERT_EQ(unpublished.size(), 1u);
    EXPECT_EQ(unpublished[0].venue_fill_id, "F2");

    JournalOrder eight = openOrder(8);
    eight.state = OrderState::Filled;
    EXPECT_EQ(reopened.recordFill(venueFill("F3", 8, 0.1), eight).local_fill_id, 3u);
}

TEST(VenueJournal, J3_CashEntryIsRecordedOnce)
{
    test_support::TempDir dir("venue-journal");
    VenueJournal journal((dir / "kraken.sqlite").string(), "kraken-futures", "t1");
    JournalCashEntry entry;
    entry.entry_id = "101:fee";
    entry.kind = VenueCostKind::Fee;
    entry.day = TODAY;
    entry.coin = "BTCUSDT";
    entry.amount_usd = -0.75;
    journal.recordCashEntry(entry);
    entry.amount_usd = -99.0; // a second record of the same id changes nothing
    journal.recordCashEntry(entry);

    EXPECT_TRUE(journal.knowsCashEntry("101:fee"));
    auto pending = journal.unpublishedCashEntries();
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_DOUBLE_EQ(pending[0].amount_usd, -0.75);
    journal.markCashEntryPublished("101:fee");
    EXPECT_TRUE(journal.unpublishedCashEntries().empty());
}
