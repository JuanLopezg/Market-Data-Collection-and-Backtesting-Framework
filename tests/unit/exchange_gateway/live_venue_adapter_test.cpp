// LiveVenueAdapter against a fake Kraken venue (tests/specs/exchange_gateway.md, Part A).

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "test_support.h"
#include "venue_fakes.h"

using namespace venue_fakes;

namespace {

SubmitOrderCommand orderCommand(OrderID id, const Coin& coin, OrderSide side, double quantity, double referencePrice,
                                Timestamp activeFrom = TODAY)
{
    SubmitOrderCommand command;
    command.metadata.message_id = "submit-" + std::to_string(id);
    command.order = ExecutionOrder(id, 1, TODAY - 1, activeFrom, coin, side, quantity);
    command.order.reference_price = referencePrice;
    return command;
}

SubmitOrderCommand stopCommand(OrderID id, double quantity, double stopPrice, OrderSide side = OrderSide::Sell)
{
    SubmitOrderCommand command;
    command.metadata.message_id = "stop-" + std::to_string(id);
    command.order = ExecutionOrder(id, 1, TODAY, TODAY, "BTCUSDT", side, quantity);
    command.order.reference_price = 62000.0;
    command.order.position_stop_price = stopPrice;
    return command;
}

CancelOrderCommand cancelCommand(OrderID id)
{
    CancelOrderCommand command;
    command.metadata.message_id = "cancel-" + std::to_string(id);
    command.order_id = id;
    command.requested_at = TODAY;
    return command;
}

std::string clientId(OrderID id) { return "algo-" + NAMESPACE + "-" + std::to_string(id); }

// One adapter over a fake venue, a real SQLite journal and explicit clocks.
class VenueAdapter : public ::testing::Test {
protected:
    test_support::TempDir dir{"venue-adapter"};
    ScriptedTransport transport;
    KrakenFuturesProtocol protocol{HOST, CREDENTIALS};
    std::unique_ptr<VenueJournal> journal;
    std::unique_ptr<GetOnlyTransport> get_only;
    std::unique_ptr<LiveVenueAdapter> adapter;
    Events events;
    Timestamp today = TODAY;
    long long now_ms = T0_MS;
    LiveVenueOptions options;

    std::string journalPath() const { return (dir / "kraken.sqlite").string(); }

    void start(bool submissionEnabled, SymbolMap map = testSymbolMap())
    {
        options.order_submission_enabled = submissionEnabled;
        options.account_namespace = NAMESPACE;
        if (!journal)
            journal = std::make_unique<VenueJournal>(journalPath(), "kraken-futures", NAMESPACE);
        HttpTransport* used = &transport;
        if (!submissionEnabled) { // as the gateway wires it
            get_only = std::make_unique<GetOnlyTransport>(transport);
            used = get_only.get();
        }
        adapter = std::make_unique<LiveVenueAdapter>(
            protocol, *used, *journal, std::move(map), [this] { return today; }, [this] { return now_ms; }, options);
        adapter->setHandlers(events.handlers());
    }

    // A new process: new adapter and journal connection over the same file, new events.
    void restart(bool submissionEnabled = true)
    {
        adapter.reset();
        journal.reset();
        events = Events{};
        now_ms += 1000;
        start(submissionEnabled);
    }

    void pollAfter(long long ms)
    {
        now_ms += ms;
        adapter->poll(100);
    }

    JournalOrder stored(OrderID id) const
    {
        const auto order = journal->order(id);
        if (!order)
            throw std::runtime_error("order not journaled");
        return *order;
    }

    std::string rejectionReason(OrderID id) const
    {
        for (const OrderUpdateEvent& event : events.updates)
            if (event.update.order_id == id && event.update.status == ExecutionOrderStatus::Rejected)
                return event.update.message;
        return {};
    }

    void expectRejectedLocally(OrderID id, const std::string& reasonPrefix)
    {
        EXPECT_EQ(events.statusesOf(id), (std::vector<ExecutionOrderStatus>{ExecutionOrderStatus::Rejected}));
        EXPECT_EQ(rejectionReason(id).rfind(reasonPrefix, 0), 0u) << rejectionReason(id);
        EXPECT_EQ(transport.count("sendorder"), 0);
        EXPECT_EQ(stored(id).state, OrderState::Rejected);
    }
};

} // namespace


// ---------------------------------------------------------------- A1

TEST_F(VenueAdapter, A1_DefaultOptionsRejectEveryOrderLocally)
{
    EXPECT_FALSE(LiveVenueOptions{}.order_submission_enabled);
    start(false);
    adapter->submitOrder(orderCommand(7, "BTCUSDT", OrderSide::Buy, 0.1, 62000.0));
    pollAfter(1000);
    pollAfter(20000);
    EXPECT_EQ(events.statusesOf(7), (std::vector<ExecutionOrderStatus>{ExecutionOrderStatus::Rejected}));
    EXPECT_NE(rejectionReason(7).find("order submission disabled"), std::string::npos);
    EXPECT_EQ(transport.posts(), 0);
    for (const HttpRequest& request : transport.requests)
        EXPECT_EQ(request.method, "GET") << request.endpoint;
}

TEST_F(VenueAdapter, A1_GetOnlyTransportRefusesPostsAndPassesGets)
{
    GetOnlyTransport guard(transport);
    KrakenFuturesProtocol kraken(HOST, CREDENTIALS);
    EXPECT_THROW(guard.send(kraken.submitRequest({7, clientId(7), "PF_XBTUSD", OrderSide::Buy, 0.1})),
                 std::logic_error);
    EXPECT_THROW(guard.send(kraken.cancelRequest(clientId(7))), std::logic_error);
    EXPECT_EQ(transport.requests.size(), 0u);
    EXPECT_TRUE(guard.send(kraken.fillsRequest()).transport_ok);
    EXPECT_EQ(transport.count("fills"), 1);
}

// ---------------------------------------------------------------- A2 / A3

TEST_F(VenueAdapter, A2_OrderIsJournaledBeforeItIsSent)
{
    start(true);
    std::optional<OrderState> stateWhenSent;
    transport.on_send = [&](const HttpRequest& request) {
        if (request.endpoint == "sendorder")
            stateWhenSent = journal->order(7)->state;
    };
    transport.script("sendorder", {ok(placed())});

    adapter->submitOrder(orderCommand(7, "BTCUSDT", OrderSide::Buy, 0.1, 62000.0));
    EXPECT_EQ(stored(7).state, OrderState::Prepared);
    EXPECT_EQ(transport.count("sendorder"), 0);
    EXPECT_TRUE(events.updates.empty());

    pollAfter(1000);
    ASSERT_TRUE(stateWhenSent.has_value());
    EXPECT_EQ(*stateWhenSent, OrderState::Sending);
}

TEST_F(VenueAdapter, A3_AcceptedOrderIsReportedOnceWithDeterministicId)
{
    start(true);
    transport.script("sendorder", {ok(placed("venue-order-1"))});
    transport.script("openorders", {ok(openOrdersBody({openOrderRow(clientId(7), "PF_XBTUSD", "buy", "0", "0.1")}))});
    adapter->submitOrder(orderCommand(7, "BTCUSDT", OrderSide::Buy, 0.1, 62000.0));
    pollAfter(1000);
    pollAfter(5000);
    pollAfter(5000);

    EXPECT_EQ(transport.count("sendorder"), 1);
    EXPECT_EQ(stored(7).state, OrderState::Open);
    EXPECT_EQ(stored(7).venue_order_id, "venue-order-1");
    ASSERT_EQ(events.updates.size(), 1u);
    EXPECT_EQ(events.updates[0].update.status, ExecutionOrderStatus::Accepted);
    EXPECT_EQ(events.updates[0].update.exchange_order_id, "venue-order-1");
    EXPECT_EQ(events.updates[0].metadata.message_id, "kraken-futures-order:7:OPEN");

    const HttpRequest sent = transport.requestsTo("sendorder").front();
    EXPECT_EQ(formValue(sent.body, "orderType"), "mkt");
    EXPECT_EQ(formValue(sent.body, "symbol"), "PF_XBTUSD");
    EXPECT_EQ(formValue(sent.body, "side"), "buy");
    EXPECT_EQ(formValue(sent.body, "size"), "0.1");
    EXPECT_EQ(formValue(sent.body, "cliOrdId"), clientId(7));
}

// ---------------------------------------------------------------- A4

TEST_F(VenueAdapter, A4_OrderIsHeldUntilItsBusinessDay)
{
    start(true);
    transport.script("sendorder", {ok(placed())});
    adapter->submitOrder(orderCommand(7, "BTCUSDT", OrderSide::Buy, 0.1, 62000.0, TODAY + 1));
    pollAfter(1000);
    pollAfter(60000);
    EXPECT_EQ(transport.count("sendorder"), 0);
    EXPECT_EQ(stored(7).state, OrderState::Prepared);

    today = TODAY + 1;
    pollAfter(1000);
    EXPECT_EQ(transport.count("sendorder"), 1);
    EXPECT_EQ(stored(7).state, OrderState::Open);
}

// ---------------------------------------------------------------- A5

TEST_F(VenueAdapter, A5_UnmappedSymbolIsMarketUnavailable)
{
    start(true);
    adapter->submitOrder(orderCommand(7, "XYZUSDT", OrderSide::Buy, 10.0, 1.0));
    pollAfter(1000);
    expectRejectedLocally(7, "MARKET_UNAVAILABLE");
}

TEST_F(VenueAdapter, A5_MappedButUnlistedContractIsMarketUnavailable)
{
    SymbolMap map("kraken-futures", {{"BTCUSDT", "BTC", "PF_XBTUSD", 1.0}, {"GONEUSDT", "GONE", "PF_GONEUSD", 1.0}});
    start(true, std::move(map));
    adapter->submitOrder(orderCommand(7, "GONEUSDT", OrderSide::Buy, 10.0, 1.0));
    pollAfter(1000);
    expectRejectedLocally(7, "MARKET_UNAVAILABLE");
}

TEST_F(VenueAdapter, A5_ExpiredContractIsMarketNotTrading)
{
    start(true);
    adapter->submitOrder(orderCommand(7, "OLDUSDT", OrderSide::Buy, 10.0, 1.0));
    pollAfter(1000);
    expectRejectedLocally(7, "MARKET_NOT_TRADING");
}

TEST_F(VenueAdapter, A5_QuantityBelowTheStepIsRejected)
{
    start(true);
    adapter->submitOrder(orderCommand(7, "BTCUSDT", OrderSide::Buy, 0.00004, 62000.0));
    pollAfter(1000);
    expectRejectedLocally(7, "BELOW_MINIMUM_QUANTITY");
}

// ---------------------------------------------------------------- A6

TEST_F(VenueAdapter, A6_UsdValueWithinOneAndAHalfTimesPasses)
{
    start(true);
    transport.script("sendorder", {ok(placed("v-7")), ok(placed("v-8"))});
    adapter->submitOrder(orderCommand(7, "BTCUSDT", OrderSide::Buy, 0.1, 62000.0));
    adapter->submitOrder(orderCommand(8, "BTCUSDT", OrderSide::Buy, 0.1, 45000.0)); // ratio 1.378
    pollAfter(1000);
    EXPECT_EQ(transport.count("sendorder"), 2);
    EXPECT_EQ(stored(7).state, OrderState::Open);
    EXPECT_EQ(stored(8).state, OrderState::Open);
}

TEST_F(VenueAdapter, A6_UsdValueOutsideOneAndAHalfTimesIsRejected)
{
    start(true);
    adapter->submitOrder(orderCommand(7, "BTCUSDT", OrderSide::Buy, 0.1, 40000.0)); // ratio 1.55
    adapter->submitOrder(orderCommand(8, "BTCUSDT", OrderSide::Buy, 0.1, 95000.0)); // ratio 0.653
    pollAfter(1000);
    expectRejectedLocally(7, "NOTIONAL_MISMATCH");
    expectRejectedLocally(8, "NOTIONAL_MISMATCH");
}

TEST_F(VenueAdapter, A6_WrongUnitScaleNeverReachesTheVenue)
{
    start(true, testSymbolMap(1.0)); // 1000PEPEUSDT wrongly mapped 1:1
    adapter->submitOrder(orderCommand(7, "1000PEPEUSDT", OrderSide::Buy, 5.0, 0.01));
    pollAfter(1000);
    expectRejectedLocally(7, "NOTIONAL_MISMATCH");
}

TEST_F(VenueAdapter, A6_MissingPricesAreRejected)
{
    start(true);
    adapter->submitOrder(orderCommand(7, "BTCUSDT", OrderSide::Buy, 0.1, 0.0));
    adapter->submitOrder(orderCommand(8, "NOPXUSDT", OrderSide::Buy, 10.0, 1.0));
    pollAfter(1000);
    expectRejectedLocally(7, "NO_REFERENCE_PRICE");
    expectRejectedLocally(8, "NO_VENUE_PRICE");
}

// ---------------------------------------------------------------- A7 / A8

TEST_F(VenueAdapter, A7_QuantityIsRoundedToTheNearestStep)
{
    start(true);
    transport.script("sendorder", {ok(placed()), ok(placed())});
    adapter->submitOrder(orderCommand(7, "BTCUSDT", OrderSide::Buy, 0.12349, 62000.0));
    adapter->submitOrder(orderCommand(8, "BTCUSDT", OrderSide::Buy, 0.12344, 62000.0));
    pollAfter(1000);
    ASSERT_EQ(transport.count("sendorder"), 2);
    EXPECT_EQ(formValue(transport.requestsTo("sendorder")[0].body, "size"), "0.1235");
    EXPECT_NEAR(stored(7).venue_quantity, 0.1235, 1e-12);
    EXPECT_EQ(formValue(transport.requestsTo("sendorder")[1].body, "size"), "0.1234");
    EXPECT_NEAR(stored(8).venue_quantity, 0.1234, 1e-12);
}

TEST_F(VenueAdapter, A8_UnitsAreConvertedToTheVenueAndBack)
{
    start(true);
    transport.script("sendorder", {ok(placed())});
    transport.script("fills", {ok(fillsBody({fillRow("F1", clientId(7), "PF_PEPEUSD", "buy", "5000", "0.00001")}))});
    adapter->submitOrder(orderCommand(7, "1000PEPEUSDT", OrderSide::Buy, 5.0, 0.01));
    pollAfter(1000);

    ASSERT_EQ(transport.count("sendorder"), 1);
    const HttpRequest sent = transport.requestsTo("sendorder").front();
    EXPECT_EQ(formValue(sent.body, "symbol"), "PF_PEPEUSD");
    EXPECT_EQ(formValue(sent.body, "size"), "5000");

    ASSERT_EQ(events.fills.size(), 1u);
    const Fill& fill = events.fills[0].fill;
    EXPECT_EQ(fill.coin, "1000PEPEUSDT");
    EXPECT_NEAR(fill.quantity, 5.0, 1e-12);
    EXPECT_NEAR(fill.price, 0.01, 1e-15);
    EXPECT_NEAR(fill.quantity * fill.price, 0.05, 1e-15);
    EXPECT_DOUBLE_EQ(fill.commission, 0.0);
    EXPECT_EQ(fill.order_id, 7u);
    EXPECT_EQ(fill.strategy_id, 1u);
    EXPECT_EQ(fill.side, OrderSide::Buy);
    EXPECT_EQ(fill.timestamp, TODAY);
}

// ---------------------------------------------------------------- A9

TEST_F(VenueAdapter, A9_DuplicateCommandIsIgnored)
{
    start(true);
    transport.script("sendorder", {ok(placed())});
    adapter->submitOrder(orderCommand(7, "BTCUSDT", OrderSide::Buy, 0.1, 62000.0));
    adapter->submitOrder(orderCommand(7, "BTCUSDT", OrderSide::Buy, 0.1, 62000.0));
    pollAfter(1000);
    adapter->submitOrder(orderCommand(7, "BTCUSDT", OrderSide::Buy, 0.1, 62000.0));
    pollAfter(3000);
    EXPECT_EQ(transport.count("sendorder"), 1);
}

TEST_F(VenueAdapter, A9_SameIdWithDifferentTermsIsRefused)
{
    start(true);
    adapter->submitOrder(orderCommand(7, "BTCUSDT", OrderSide::Buy, 0.1, 62000.0));
    EXPECT_THROW(adapter->submitOrder(orderCommand(7, "BTCUSDT", OrderSide::Buy, 0.2, 62000.0)), std::invalid_argument);
    EXPECT_THROW(adapter->submitOrder(orderCommand(7, "BTCUSDT", OrderSide::Sell, 0.1, 62000.0)), std::invalid_argument);
    EXPECT_THROW(adapter->submitOrder(orderCommand(7, "ETHUSDT", OrderSide::Buy, 0.1, 2500.0)), std::invalid_argument);
}

// ---------------------------------------------------------------- A10

TEST_F(VenueAdapter, A10_TimeoutBecomesUnknownAndIsNeverResent)
{
    start(true);
    transport.script("sendorder", {noAnswer(), ok(placed())});
    adapter->submitOrder(orderCommand(7, "BTCUSDT", OrderSide::Buy, 0.1, 62000.0));
    pollAfter(1000);
    EXPECT_EQ(stored(7).state, OrderState::Unknown);
    pollAfter(5000);
    pollAfter(180000); // past the 2-minute alert
    pollAfter(5000);
    EXPECT_EQ(transport.count("sendorder"), 1);
    EXPECT_EQ(stored(7).state, OrderState::Unknown);
    EXPECT_TRUE(events.updates.empty()) << "UNKNOWN is not reported to execution";
}

TEST_F(VenueAdapter, A10_ServerErrorIsUnknownAndResolvedByOpenOrders)
{
    start(true);
    transport.script("sendorder", {httpStatus(502)});
    adapter->submitOrder(orderCommand(7, "BTCUSDT", OrderSide::Buy, 0.1, 62000.0));
    pollAfter(1000);
    EXPECT_EQ(stored(7).state, OrderState::Unknown);

    transport.script("openorders",
                     {ok(openOrdersBody({openOrderRow(clientId(7), "PF_XBTUSD", "buy", "0", "0.1", "v-77")}))});
    pollAfter(5000);
    EXPECT_EQ(stored(7).state, OrderState::Open);
    EXPECT_EQ(stored(7).venue_order_id, "v-77");
    EXPECT_EQ(events.statusesOf(7), (std::vector<ExecutionOrderStatus>{ExecutionOrderStatus::Accepted}));
    EXPECT_EQ(transport.count("sendorder"), 1);
}

TEST_F(VenueAdapter, A10_UnknownIsResolvedByItsFills)
{
    start(true);
    transport.script("sendorder", {noAnswer()});
    adapter->submitOrder(orderCommand(7, "BTCUSDT", OrderSide::Buy, 0.1, 62000.0));
    pollAfter(1000);
    transport.script("fills", {ok(fillsBody({fillRow("F1", clientId(7), "PF_XBTUSD", "buy", "0.1", "62010")}))});
    pollAfter(5000);
    EXPECT_EQ(stored(7).state, OrderState::Filled);
    ASSERT_EQ(events.fills.size(), 1u);
    EXPECT_NEAR(events.fills[0].fill.quantity, 0.1, 1e-12);
    EXPECT_EQ(events.statusesOf(7), (std::vector<ExecutionOrderStatus>{ExecutionOrderStatus::Filled}));
    EXPECT_EQ(transport.count("sendorder"), 1);
}

// ---------------------------------------------------------------- A11

TEST_F(VenueAdapter, A11_NotProcessedIsResentWithTheSameClientId)
{
    start(true);
    transport.script("sendorder", {httpStatus(429), ok(placed())});
    adapter->submitOrder(orderCommand(7, "BTCUSDT", OrderSide::Buy, 0.1, 62000.0));
    pollAfter(1000);
    EXPECT_EQ(stored(7).state, OrderState::Prepared);
    pollAfter(2000);
    ASSERT_EQ(transport.count("sendorder"), 2);
    const auto sent = transport.requestsTo("sendorder");
    EXPECT_EQ(formValue(sent[0].body, "cliOrdId"), clientId(7));
    EXPECT_EQ(formValue(sent[1].body, "cliOrdId"), clientId(7));
    EXPECT_EQ(stored(7).state, OrderState::Open);
}

TEST_F(VenueAdapter, A11_ApiLimitIsResent)
{
    start(true);
    transport.script("sendorder", {ok(venueError("apiLimitExceeded")), ok(placed())});
    adapter->submitOrder(orderCommand(7, "BTCUSDT", OrderSide::Buy, 0.1, 62000.0));
    pollAfter(1000);
    pollAfter(2000);
    EXPECT_EQ(transport.count("sendorder"), 2);
    EXPECT_EQ(stored(7).state, OrderState::Open);
}

// ---------------------------------------------------------------- A12

TEST_F(VenueAdapter, A12_CrashDuringSendBecomesUnknownNotResent)
{
    start(true);
    adapter->submitOrder(orderCommand(7, "BTCUSDT", OrderSide::Buy, 0.1, 62000.0));
    JournalOrder crashed = stored(7);
    crashed.state = OrderState::Sending; // the process died while the request was in flight
    crashed.sent_at_ms = now_ms;
    journal->updateOrder(crashed);

    restart();
    transport.script("sendorder", {ok(placed())});
    pollAfter(1000);
    pollAfter(5000);
    EXPECT_EQ(transport.count("sendorder"), 0);
    EXPECT_EQ(stored(7).state, OrderState::Unknown);
}

TEST_F(VenueAdapter, A12_PreparedOrderIsSentOnceAfterRestart)
{
    start(true);
    adapter->submitOrder(orderCommand(7, "BTCUSDT", OrderSide::Buy, 0.1, 62000.0));
    restart();
    transport.script("sendorder", {ok(placed())});
    pollAfter(1000);
    pollAfter(3000);
    EXPECT_EQ(transport.count("sendorder"), 1);
    EXPECT_EQ(stored(7).state, OrderState::Open);
}

TEST_F(VenueAdapter, A12_OpenOrderIsNotResentAndItsFillIsPublishedOnce)
{
    start(true);
    transport.script("sendorder", {ok(placed())});
    transport.script("openorders", {ok(openOrdersBody({openOrderRow(clientId(7), "PF_XBTUSD", "buy", "0", "0.1")}))});
    adapter->submitOrder(orderCommand(7, "BTCUSDT", OrderSide::Buy, 0.1, 62000.0));
    pollAfter(1000);
    ASSERT_EQ(stored(7).state, OrderState::Open);

    restart();
    transport.script("fills", {ok(fillsBody({fillRow("F1", clientId(7), "PF_XBTUSD", "buy", "0.1", "62010")}))});
    transport.script("openorders", {ok(openOrdersBody({}))});
    pollAfter(1000);
    pollAfter(5000);
    restart();
    pollAfter(5000);

    EXPECT_EQ(transport.count("sendorder"), 1);
    EXPECT_EQ(stored(7).state, OrderState::Filled);
    EXPECT_TRUE(events.fills.empty()) << "already published before the second restart";
    EXPECT_TRUE(journal->knowsFill("F1"));
    EXPECT_TRUE(journal->unpublishedFills().empty());
}

// ---------------------------------------------------------------- A13 / A14

TEST_F(VenueAdapter, A13_SameVenueFillIsPublishedOnce)
{
    start(true);
    transport.script("sendorder", {ok(placed())});
    transport.script("fills", {ok(fillsBody({fillRow("F1", clientId(7), "PF_XBTUSD", "buy", "0.1", "62010")}))});
    adapter->submitOrder(orderCommand(7, "BTCUSDT", OrderSide::Buy, 0.1, 62000.0));
    pollAfter(1000);
    pollAfter(5000);
    pollAfter(5000);
    ASSERT_EQ(events.fills.size(), 1u);
    EXPECT_EQ(events.fills[0].fill.fill_id, 1u);
    EXPECT_EQ(events.fills[0].metadata.message_id, "kraken-futures-fill:F1");
}

TEST_F(VenueAdapter, A13_LocalFillIdsFollowArrivalAndFillsPrecedeTheFilledUpdate)
{
    start(true);
    transport.script("sendorder", {ok(placed("v-7")), ok(placed("v-8"))});
    adapter->submitOrder(orderCommand(7, "BTCUSDT", OrderSide::Buy, 0.1, 62000.0));
    adapter->submitOrder(orderCommand(8, "ETHUSDT", OrderSide::Sell, 2.0, 2500.0));
    transport.script("fills", {ok(fillsBody(
        {fillRow("F-ETH", clientId(8), "PF_ETHUSD", "sell", "2", "2499", "2026-10-12T00:00:08.000Z", "v-8"),
         fillRow("F-BTC", clientId(7), "PF_XBTUSD", "buy", "0.1", "62010", "2026-10-12T00:00:07.000Z", "v-7")}))});
    pollAfter(1000);

    ASSERT_EQ(events.fills.size(), 2u);
    EXPECT_EQ(events.fills[0].fill.order_id, 7u) << "oldest venue fill first";
    EXPECT_EQ(events.fills[0].fill.fill_id, 1u);
    EXPECT_EQ(events.fills[1].fill.order_id, 8u);
    EXPECT_EQ(events.fills[1].fill.fill_id, 2u);
    EXPECT_EQ(events.fills[1].fill.side, OrderSide::Sell);

    for (const OrderID id : {7u, 8u}) {
        const auto fillAt = std::find(events.sequence.begin(), events.sequence.end(), "fill:" + std::to_string(id));
        const auto filledAt = std::find(events.sequence.begin(), events.sequence.end(),
                                        "update:" + std::to_string(id) + ":filled");
        ASSERT_NE(fillAt, events.sequence.end());
        ASSERT_NE(filledAt, events.sequence.end());
        EXPECT_LT(fillAt - events.sequence.begin(), filledAt - events.sequence.begin()) << "order " << id;
    }
}

TEST_F(VenueAdapter, A14_PartialFillsAddUpToTheOrder)
{
    start(true);
    transport.script("sendorder", {ok(placed())});
    transport.script("openorders", {ok(openOrdersBody({openOrderRow(clientId(7), "PF_XBTUSD", "buy", "0.04", "0.06")}))});
    transport.script("fills", {ok(fillsBody({fillRow("F1", clientId(7), "PF_XBTUSD", "buy", "0.04", "62000")}))});
    adapter->submitOrder(orderCommand(7, "BTCUSDT", OrderSide::Buy, 0.1, 62000.0));
    pollAfter(1000);
    EXPECT_EQ(stored(7).state, OrderState::Open);
    ASSERT_EQ(events.fills.size(), 1u);
    EXPECT_NEAR(events.fills[0].fill.quantity, 0.04, 1e-12);
    EXPECT_EQ(events.statusesOf(7), (std::vector<ExecutionOrderStatus>{ExecutionOrderStatus::Accepted}));

    transport.script("fills", {ok(fillsBody({fillRow("F2", clientId(7), "PF_XBTUSD", "buy", "0.06", "62020",
                                                     "2026-10-12T00:00:09.000Z"),
                                             fillRow("F1", clientId(7), "PF_XBTUSD", "buy", "0.04", "62000")}))});
    transport.script("openorders", {ok(openOrdersBody({}))});
    pollAfter(5000);
    EXPECT_EQ(stored(7).state, OrderState::Filled);
    ASSERT_EQ(events.fills.size(), 2u);
    EXPECT_NEAR(events.fills[0].fill.quantity + events.fills[1].fill.quantity, 0.1, 1e-12);
    EXPECT_DOUBLE_EQ(events.fills[1].fill.price, 62020.0);
    EXPECT_EQ(events.statusesOf(7),
              (std::vector<ExecutionOrderStatus>{ExecutionOrderStatus::Accepted, ExecutionOrderStatus::Filled}));
}

// ---------------------------------------------------------------- A15

TEST_F(VenueAdapter, A15_FillsThatAreNotOursAreNotAttributed)
{
    start(true);
    transport.script("sendorder", {ok(placed("v-7"))});
    transport.script("openorders", {ok(openOrdersBody({openOrderRow(clientId(7), "PF_XBTUSD", "buy", "0", "0.1", "v-7")}))});
    transport.script("fills", {ok(fillsBody(
        {fillRow("X1", "algo-other-7", "PF_XBTUSD", "buy", "0.1", "62000", "2026-10-12T00:00:07.000Z", "v-7"),
         fillRow("X2", "", "PF_XBTUSD", "buy", "0.1", "62000", "2026-10-12T00:00:07.000Z", "v-manual"),
         fillRow("X3", "", "PF_ETHUSD", "sell", "1", "2500", "2026-10-12T00:00:07.000Z", "v-liquidation")}))});
    adapter->submitOrder(orderCommand(7, "BTCUSDT", OrderSide::Buy, 0.1, 62000.0));
    pollAfter(1000);
    pollAfter(5000);
    EXPECT_TRUE(events.fills.empty());
    EXPECT_EQ(stored(7).state, OrderState::Open);
    EXPECT_DOUBLE_EQ(stored(7).filled_venue_quantity, 0.0);
}

// ---------------------------------------------------------------- A16

TEST_F(VenueAdapter, A16_PreparedOrderIsCancelledLocally)
{
    start(true);
    adapter->submitOrder(orderCommand(7, "BTCUSDT", OrderSide::Buy, 0.1, 62000.0, TODAY + 1));
    adapter->cancelOrder(cancelCommand(7));
    today = TODAY + 1;
    pollAfter(1000);
    EXPECT_EQ(transport.count("sendorder"), 0);
    EXPECT_EQ(transport.count("cancelorder"), 0);
    EXPECT_EQ(stored(7).state, OrderState::Cancelled);
    EXPECT_EQ(events.statusesOf(7), (std::vector<ExecutionOrderStatus>{ExecutionOrderStatus::Canceled}));
}

TEST_F(VenueAdapter, A16_OpenOrderIsCancelledAtTheVenueByClientId)
{
    start(true);
    transport.script("sendorder", {ok(placed())});
    transport.script("openorders", {ok(openOrdersBody({openOrderRow(clientId(7), "PF_XBTUSD", "buy", "0", "0.1")}))});
    transport.script("cancelorder", {ok(R"({"result":"success","cancelStatus":{"status":"cancelled"}})")});
    adapter->submitOrder(orderCommand(7, "BTCUSDT", OrderSide::Buy, 0.1, 62000.0));
    pollAfter(1000);
    adapter->cancelOrder(cancelCommand(7));
    pollAfter(1000);
    ASSERT_EQ(transport.count("cancelorder"), 1);
    EXPECT_EQ(transport.requestsTo("cancelorder").front().body, "cliOrdId=" + clientId(7));
    EXPECT_EQ(stored(7).state, OrderState::Cancelled);
    EXPECT_EQ(events.statusesOf(7),
              (std::vector<ExecutionOrderStatus>{ExecutionOrderStatus::Accepted, ExecutionOrderStatus::Canceled}));
}

// ---------------------------------------------------------------- A17

TEST_F(VenueAdapter, A17_PositionStopPriceIsRoundedDownToTheTick)
{
    start(true);
    transport.script("sendorder", {ok(placed())});
    adapter->submitOrder(stopCommand(9, 0.1, 43400.7));
    pollAfter(1000);
    ASSERT_EQ(transport.count("sendorder"), 1);
    const std::string body = transport.requestsTo("sendorder").front().body;
    EXPECT_EQ(formValue(body, "orderType"), "stp");
    EXPECT_EQ(formValue(body, "side"), "sell");
    EXPECT_DOUBLE_EQ(std::stod(formValue(body, "stopPrice")), 43400.0);
    EXPECT_EQ(formValue(body, "reduceOnly"), "true");
    EXPECT_EQ(formValue(body, "triggerSignal"), "mark");
    EXPECT_FALSE(formHas(body, "limitPrice"));
    EXPECT_DOUBLE_EQ(stored(9).venue_stop_price, 43400.0);
}

TEST_F(VenueAdapter, A17_StopAtOrAboveTheMarkIsRejected)
{
    start(true);
    adapter->submitOrder(stopCommand(9, 0.1, 63000.0));
    pollAfter(1000);
    expectRejectedLocally(9, "STOP_NOT_BELOW_MARK");
}

TEST_F(VenueAdapter, A17_BuyStopIsRejected)
{
    start(true);
    adapter->submitOrder(stopCommand(9, 0.1, 43400.0, OrderSide::Buy));
    pollAfter(1000);
    expectRejectedLocally(9, "INVALID_POSITION_STOP");
}

// ---------------------------------------------------------------- A18

TEST_F(VenueAdapter, A18_WithdrawalKeyIsRefused)
{
    start(false);
    transport.script("permissions", {ok(R"({"permissions":{"general":"READ_ONLY","withdrawal":"FULL_ACCESS"}})")});
    EXPECT_THROW(adapter->verifyPermissions(), std::runtime_error);
}

TEST_F(VenueAdapter, A18_EnabledSubmissionNeedsATradingKey)
{
    start(true);
    transport.script("permissions", {ok(R"({"permissions":{"general":"READ_ONLY"}})")});
    EXPECT_THROW(adapter->verifyPermissions(), std::runtime_error);
    transport.script("permissions", {ok(R"({"permissions":{"general":"FULL_ACCESS"}})")});
    EXPECT_TRUE(adapter->verifyPermissions().trade);
}

// ---------------------------------------------------------------- A19

TEST_F(VenueAdapter, A19_SnapshotInSourceUnitsWithSpotStyleCash)
{
    start(false);
    transport.script("accounts", {ok(R"({"result":"success","accounts":{"flex":{"type":"multiCollateralMarginAccount",
        "currencies":{"USD":{"quantity":10000.0},"XBT":{"quantity":0.01,"collateral":613.8}}}}})")});
    transport.script("openpositions", {ok(R"({"result":"success","openPositions":[
        {"symbol":"PF_XBTUSD","side":"long","size":0.1,"price":60000.0},
        {"symbol":"PF_PEPEUSD","side":"short","size":2000,"price":0.00001}]})")});
    ExchangeSnapshotRequest request;
    request.metadata.message_id = "snap-1";
    adapter->requestSnapshot(request);

    ASSERT_EQ(events.snapshots.size(), 1u);
    const auto& snapshot = events.snapshots[0].snapshot;
    EXPECT_NEAR(snapshot.positions.at("BTCUSDT"), 0.1, 1e-12);
    EXPECT_NEAR(snapshot.positions.at("1000PEPEUSDT"), -2.0, 1e-12);
    EXPECT_NEAR(snapshot.cash, 4000.02, 1e-9);
    EXPECT_NEAR(snapshot.collateral_usd, 613.8, 1e-12);
    EXPECT_EQ(snapshot.timestamp, TODAY);
    EXPECT_EQ(transport.posts(), 0);
}

// ---------------------------------------------------------------- A15 (venue id fallback, reused id)

TEST_F(VenueAdapter, A15_FillWithoutClientIdIsMatchedByJournaledVenueOrderId)
{
    start(true);
    transport.script("sendorder", {ok(placed("v-7"))});
    adapter->submitOrder(orderCommand(7, "BTCUSDT", OrderSide::Buy, 0.1, 62000.0));
    transport.script("fills", {ok(fillsBody({fillRow("F1", "", "PF_XBTUSD", "buy", "0.1", "62010",
                                                     "2026-10-12T00:00:07.000Z", "v-7")}))});
    pollAfter(1000);
    ASSERT_EQ(events.fills.size(), 1u);
    EXPECT_EQ(events.fills[0].fill.order_id, 7u);
    EXPECT_EQ(stored(7).state, OrderState::Filled);
}

TEST_F(VenueAdapter, A15_FillOlderThanItsOrderIsNotAttributed)
{
    start(true);
    transport.script("sendorder", {ok(placed("v-7"))});
    transport.script("openorders", {ok(openOrdersBody({openOrderRow(clientId(7), "PF_XBTUSD", "buy", "0", "0.1", "v-7")}))});
    adapter->submitOrder(orderCommand(7, "BTCUSDT", OrderSide::Buy, 0.1, 62000.0));
    transport.script("fills", {ok(fillsBody({fillRow("OLD", clientId(7), "PF_XBTUSD", "buy", "0.1", "50000",
                                                     "2026-10-11T12:00:00.000Z", "v-7")}))});
    pollAfter(1000);
    EXPECT_TRUE(events.fills.empty());
    EXPECT_EQ(stored(7).state, OrderState::Open);
}

// ---------------------------------------------------------------- A20

TEST_F(VenueAdapter, A20_FeesAndFundingArePublishedOnceAfterTheBaseline)
{
    const std::string baseline = R"({"logs":[
     {"id":50,"date":"2026-10-11T10:00:00.000Z","asset":"usd","info":"futures trade","fee":1.0,"execution":"OLD-FILL",
      "realized_funding":null,"contract":"pf_xbtusd","old_balance":1.0,"new_balance":0.0,"margin_account":"flex"}]})";
    const std::string later = R"({"logs":[
     {"id":52,"date":"2026-10-12T08:00:00.000Z","asset":"usd","info":"futures trade","fee":null,"execution":null,
      "realized_funding":-0.2,"contract":"pf_xbtusd","old_balance":999.25,"new_balance":999.05,"margin_account":"flex"},
     {"id":51,"date":"2026-10-12T00:00:08.000Z","asset":"usd","info":"futures trade","fee":0.75,"execution":"F1",
      "realized_funding":null,"contract":"pf_xbtusd","old_balance":1000.0,"new_balance":999.25,"margin_account":"flex"},
     {"id":50,"date":"2026-10-11T10:00:00.000Z","asset":"usd","info":"futures trade","fee":1.0,"execution":"OLD-FILL",
      "realized_funding":null,"contract":"pf_xbtusd","old_balance":1.0,"new_balance":0.0,"margin_account":"flex"}]})";
    start(true);
    transport.script("account-log", {ok(baseline)});
    pollAfter(1000);
    EXPECT_TRUE(events.costs.empty()) << "the first read is only a baseline";

    transport.script("sendorder", {ok(placed())});
    transport.script("fills", {ok(fillsBody({fillRow("F1", clientId(7), "PF_XBTUSD", "buy", "0.1", "62010")}))});
    adapter->submitOrder(orderCommand(7, "BTCUSDT", OrderSide::Buy, 0.1, 62000.0));
    pollAfter(1000);
    ASSERT_EQ(events.fills.size(), 1u);

    transport.script("account-log", {ok(later)});
    pollAfter(400000);
    pollAfter(400000);
    ASSERT_EQ(events.costs.size(), 2u);
    std::map<VenueCostKind, VenueCostEvent> byKind;
    for (const VenueCostEvent& cost : events.costs)
        byKind[cost.kind] = cost;
    ASSERT_TRUE(byKind.contains(VenueCostKind::Fee));
    EXPECT_DOUBLE_EQ(byKind[VenueCostKind::Fee].amount_usd, -0.75);
    EXPECT_EQ(byKind[VenueCostKind::Fee].fill_id, events.fills[0].fill.fill_id);
    EXPECT_EQ(byKind[VenueCostKind::Fee].coin, "BTCUSDT");
    ASSERT_TRUE(byKind.contains(VenueCostKind::Funding));
    EXPECT_DOUBLE_EQ(byKind[VenueCostKind::Funding].amount_usd, -0.2);
    EXPECT_EQ(byKind[VenueCostKind::Funding].coin, "BTCUSDT");
    EXPECT_EQ(byKind[VenueCostKind::Funding].timestamp, TODAY);
}

// ---------------------------------------------------------------- A21

TEST_F(VenueAdapter, A21_OpenOrderNoLongerListedEndsCancelled)
{
    start(true);
    transport.script("sendorder", {ok(placed())});
    adapter->submitOrder(orderCommand(7, "BTCUSDT", OrderSide::Buy, 0.1, 62000.0));
    pollAfter(1000);
    ASSERT_EQ(stored(7).state, OrderState::Open);
    pollAfter(5000);
    EXPECT_EQ(stored(7).state, OrderState::Open) << "not yet 15 s without evidence";
    pollAfter(11000);
    EXPECT_EQ(stored(7).state, OrderState::Cancelled);
    EXPECT_EQ(events.statusesOf(7),
              (std::vector<ExecutionOrderStatus>{ExecutionOrderStatus::Accepted, ExecutionOrderStatus::Canceled}));
}

TEST_F(VenueAdapter, A21_PartiallyFilledOrderEndsWithItsFillsFirst)
{
    start(true);
    transport.script("sendorder", {ok(placed())});
    transport.script("fills", {ok(fillsBody({fillRow("F1", clientId(7), "PF_XBTUSD", "buy", "0.04", "62000")}))});
    adapter->submitOrder(orderCommand(7, "BTCUSDT", OrderSide::Buy, 0.1, 62000.0));
    pollAfter(1000);
    pollAfter(20000);
    EXPECT_EQ(stored(7).state, OrderState::Cancelled);
    ASSERT_EQ(events.fills.size(), 1u);
    EXPECT_NEAR(events.fills[0].fill.quantity, 0.04, 1e-12);
    EXPECT_EQ(events.sequence.back(), "update:7:canceled");
}
