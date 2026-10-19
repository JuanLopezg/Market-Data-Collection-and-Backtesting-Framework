#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "adapter_test_support.h"
#include "adapters/backend_gateway.h"
#include "account.h"
#include "test_support.h"

namespace {
using namespace adapter_test;

struct BackendHarness {
    static constexpr const char* stream = "ADAPTER_BACKEND_TEST";
    std::unique_ptr<test_support::DockerContainer> server = test_support::DockerContainer::startNats();
    JetStreamBus peer{server->url()};
    std::unique_ptr<BackendGateway> gateway = std::make_unique<BackendGateway>(server->url(), stream);
    std::vector<OrderUpdateEvent> updates;
    std::vector<FillEvent> fills;
    std::vector<ExchangeSnapshotEvent> snapshots;
    std::vector<char> sequence;

    ExchangeGatewayHandlers handlers()
    {
        ExchangeGatewayHandlers result;
        result.on_order_update = [this](const OrderUpdateEvent& event) {
            updates.push_back(event); sequence.push_back('U');
        };
        result.on_fill = [this](const FillEvent& event) {
            fills.push_back(event); sequence.push_back('F');
        };
        result.on_snapshot = [this](const ExchangeSnapshotEvent& event) {
            snapshots.push_back(event); sequence.push_back('S');
        };
        return result;
    }

    void install() { gateway->setHandlers(handlers()); }

    template <class Predicate>
    bool until(Predicate done)
    {
        // Technical I/O polling only: economic frontier/time never advances here.
        for (int attempt = 0; attempt < 30 && !done(); ++attempt) gateway->poll(20);
        return done();
    }

    void publishFill(FillID id = 101, Timestamp at = kD2)
    {
        const FillEvent event{metadata("fill-" + std::to_string(id), at), fill(id, at)};
        peer.publish(MessageSubjects::BACKEND_FILL, MessageJson::encode(event), event.metadata.message_id);
    }

    MessageBus::SubscriptionID capture(std::vector<BusMessage>& observed)
    {
        DurableConsumerOptions options;
        options.stream = stream;
        options.durable_name = "adapters-command-observer";
        options.subject = "gateway.backend.>";
        return peer.subscribe(options, [&](const BusMessage& message) {
            observed.push_back(message);
            return DurableMessageDisposition::Ack;
        });
    }
};

TEST(BackendGatewayService, R15_CommandsKeepMetadataAndEconomicPayloadOnPrivateRoutes)
{
    if (!test_support::dockerAvailable()) GTEST_SKIP() << "Docker is not available";
    BackendHarness h;
    std::vector<BusMessage> observed;
    const auto subscription = h.capture(observed);
    SubmitOrderCommand submitted{metadata("submit-40", kD1), order()};
    submitted.order.reference_price = 125.0;
    const CancelOrderCommand canceled{metadata("cancel-40"), 40, kD2};
    const ExchangeSnapshotRequest snapshot{metadata("snapshot-40")};
    h.gateway->submitOrder(submitted);
    h.gateway->submitOrder(submitted); // Real broker deduplication, same logical ID.
    h.gateway->cancelOrder(canceled);
    h.gateway->requestSnapshot(snapshot);
    for (int attempt = 0; attempt < 30 && observed.size() < 3; ++attempt)
        h.peer.poll(subscription, 16, 20);
    ASSERT_EQ(observed.size(), 3U);
    EXPECT_EQ(observed[0].subject, MessageSubjects::BACKEND_SUBMIT_ORDER);
    EXPECT_EQ(observed[1].subject, MessageSubjects::BACKEND_CANCEL_ORDER);
    EXPECT_EQ(observed[2].subject, MessageSubjects::BACKEND_EXCHANGE_SNAPSHOT_REQUEST);
    const auto decoded = MessageJson::decodeSubmitOrderCommand(observed[0].payload);
    EXPECT_EQ(decoded.metadata.message_id, "submit-40");
    EXPECT_EQ(decoded.metadata.correlation_id, submitted.metadata.correlation_id);
    EXPECT_EQ(decoded.metadata.produced_at, kD1);
    EXPECT_EQ(decoded.order.order_id, 40U);
    EXPECT_EQ(decoded.order.side, OrderSide::Buy);
    EXPECT_DOUBLE_EQ(decoded.order.quantity, 2.0);
    EXPECT_DOUBLE_EQ(decoded.order.reference_price, 125.0);
    const auto decodedCancel = MessageJson::decodeCancelOrderCommand(observed[1].payload);
    EXPECT_EQ(decodedCancel.order_id, 40U);
    EXPECT_EQ(decodedCancel.requested_at, kD2);
    EXPECT_EQ(decodedCancel.metadata.message_id, canceled.metadata.message_id);
    EXPECT_EQ(decodedCancel.metadata.correlation_id, canceled.metadata.correlation_id);
    EXPECT_EQ(MessageJson::decodeExchangeSnapshotRequest(observed[2].payload).metadata.message_id, "snapshot-40");
    EXPECT_TRUE(h.updates.empty());
    EXPECT_TRUE(h.fills.empty());
    h.peer.close(subscription);
}

TEST(BackendGatewayService, R16_AcceptedFillFilledCallbacksPreserveOneStreamOrder)
{
    if (!test_support::dockerAvailable()) GTEST_SKIP() << "Docker is not available";
    BackendHarness h;
    h.install();
    const OrderUpdateEvent accepted{metadata("accepted-40"), {40, kD2, ExecutionOrderStatus::Accepted, "venue-40"}};
    const OrderUpdateEvent filled{metadata("filled-40"), {40, kD2, ExecutionOrderStatus::Filled, "venue-40"}};
    h.peer.publish(MessageSubjects::BACKEND_ORDER_UPDATE, MessageJson::encode(accepted), accepted.metadata.message_id);
    h.publishFill();
    h.peer.publish(MessageSubjects::BACKEND_ORDER_UPDATE, MessageJson::encode(filled), filled.metadata.message_id);
    ASSERT_TRUE(h.until([&] { return h.sequence.size() == 3; }));
    EXPECT_EQ(h.sequence, (std::vector<char>{'U', 'F', 'U'}));
    ASSERT_EQ(h.updates.size(), 2U);
    ASSERT_EQ(h.fills.size(), 1U);
    EXPECT_EQ(h.updates.front().update.status, ExecutionOrderStatus::Accepted);
    EXPECT_EQ(h.updates.back().update.status, ExecutionOrderStatus::Filled);
    EXPECT_EQ(h.fills.front().fill.fill_id, 101U);
    EXPECT_DOUBLE_EQ(h.fills.front().fill.quantity, 2.0);
    EXPECT_DOUBLE_EQ(h.fills.front().fill.price, 100.0);
    EXPECT_DOUBLE_EQ(h.fills.front().fill.commission, 2.0);
}

TEST(BackendGatewayService, R17_FutureFillWaitsForExplicitBusinessFrontier)
{
    if (!test_support::dockerAvailable()) GTEST_SKIP() << "Docker is not available";
    BackendHarness h;
    h.install();
    Timestamp frontier = kD2;
    h.gateway->setEventTimeGate([&](Timestamp at) { return at <= frontier; });
    h.publishFill(101, kD3);
    h.gateway->poll(20);
    EXPECT_TRUE(h.fills.empty());
    frontier = kD3;
    ASSERT_TRUE(h.until([&] { return h.fills.size() == 1; }));
    EXPECT_EQ(h.fills.front().metadata.produced_at, kD3);
    EXPECT_EQ(h.fills.front().fill.timestamp, kD3);
    EXPECT_EQ(h.fills.front().fill.fill_id, 101U);
}

TEST(BackendGatewayService, R17_MissingHandlersDeferFillUntilHostIsReady)
{
    if (!test_support::dockerAvailable()) GTEST_SKIP() << "Docker is not available";
    BackendHarness h;
    h.publishFill();
    h.gateway->poll(20);
    EXPECT_TRUE(h.fills.empty());
    h.install();
    ASSERT_TRUE(h.until([&] { return h.fills.size() == 1; }));
    EXPECT_EQ(h.fills.front().fill.fill_id, 101U);
}

TEST(BackendGatewayService, R16_HandlerFailureRetriesBeforeAnyEconomicApplication)
{
    if (!test_support::dockerAvailable()) GTEST_SKIP() << "Docker is not available";
    BackendHarness h;
    Account account(1000.0);
    int attempts = 0;
    auto handlers = h.handlers();
    handlers.on_fill = [&](const FillEvent& event) {
        ++attempts;
        if (attempts == 1) throw std::runtime_error("Injected pre-commit failure");
        account.applyFill(event.fill);
        h.fills.push_back(event);
    };
    h.gateway->setHandlers(std::move(handlers));
    h.publishFill();
    ASSERT_TRUE(h.until([&] { return h.fills.size() == 1; }));
    h.gateway->poll(20);
    EXPECT_EQ(attempts, 2);
    EXPECT_DOUBLE_EQ(account.cash(), 798.0);
    EXPECT_DOUBLE_EQ(account.positions().get("BTC"), 2.0);
}

TEST(BackendGatewayService, R18_RestartRetainsPendingDeliveryAndDoesNotReplayAcknowledgedFill)
{
    if (!test_support::dockerAvailable()) GTEST_SKIP() << "Docker is not available";
    BackendHarness h;
    h.install();
    h.publishFill(101, kD2);
    ASSERT_TRUE(h.until([&] { return h.fills.size() == 1; }));
    h.gateway->poll(20); // Same connection completes technical ACK traffic before restart.
    h.gateway->setEventTimeGate([](Timestamp at) { return at <= kD2; });
    h.publishFill(102, kD3);
    h.gateway->poll(20);
    EXPECT_EQ(h.fills.size(), 1U);
    h.gateway.reset();
    h.gateway = std::make_unique<BackendGateway>(h.server->url(), BackendHarness::stream);
    h.install();
    ASSERT_TRUE(h.until([&] { return h.fills.size() == 2; }));
    h.gateway->poll(20);
    ASSERT_EQ(h.fills.size(), 2U);
    EXPECT_EQ(h.fills[0].fill.fill_id, 101U);
    EXPECT_EQ(h.fills[1].fill.fill_id, 102U);
    EXPECT_EQ(h.fills[1].metadata.produced_at, kD3);
}

TEST(BackendGatewayService, R19_SnapshotFrontierPreservesCashCollateralAndPartialOrderTruth)
{
    if (!test_support::dockerAvailable()) GTEST_SKIP() << "Docker is not available";
    BackendHarness h;
    h.install();
    Timestamp frontier = kD1;
    h.gateway->setEventTimeGate([&](Timestamp at) { return at <= frontier; });
    ExchangeSnapshotEvent event;
    event.metadata = metadata("snapshot-truth-40");
    event.snapshot.timestamp = kD2;
    event.snapshot.cash = 798.0;
    event.snapshot.collateral_usd = 50.0;
    event.snapshot.positions["BTC"] = 2.0;
    event.snapshot.open_orders.push_back({40, "venue-40", "BTC", OrderSide::Buy, 5.0, 2.0});
    h.peer.publish(MessageSubjects::BACKEND_EXCHANGE_SNAPSHOT, MessageJson::encode(event), event.metadata.message_id);
    h.gateway->poll(20);
    EXPECT_TRUE(h.snapshots.empty());
    frontier = kD2;
    ASSERT_TRUE(h.until([&] { return h.snapshots.size() == 1; }));
    const auto& received = h.snapshots.front();
    EXPECT_EQ(received.metadata.message_id, event.metadata.message_id);
    EXPECT_EQ(received.metadata.correlation_id, event.metadata.correlation_id);
    EXPECT_EQ(received.snapshot.timestamp, kD2);
    EXPECT_DOUBLE_EQ(received.snapshot.cash, 798.0);
    EXPECT_DOUBLE_EQ(received.snapshot.collateral_usd, 50.0);
    EXPECT_DOUBLE_EQ(received.snapshot.positions.at("BTC"), 2.0);
    ASSERT_EQ(received.snapshot.open_orders.size(), 1U);
    EXPECT_DOUBLE_EQ(received.snapshot.open_orders.front().quantity, 5.0);
    EXPECT_DOUBLE_EQ(received.snapshot.open_orders.front().filled_quantity, 2.0);
    EXPECT_TRUE(h.fills.empty());
}

TEST(BackendGatewayService, R20_InvalidCommandsHandlersAndPollTimeoutDoNotPublish)
{
    if (!test_support::dockerAvailable()) GTEST_SKIP() << "Docker is not available";
    BackendHarness h;
    std::vector<BusMessage> observed;
    const auto subscription = h.capture(observed);
    EXPECT_THROW(h.gateway->submitOrder({metadata("invalid-submit"), order(0)}), std::invalid_argument);
    EXPECT_THROW(h.gateway->submitOrder({ContractMetadata{}, order()}), std::invalid_argument);
    EXPECT_THROW(h.gateway->cancelOrder({metadata("invalid-cancel"), 0, kD2}), std::invalid_argument);
    EXPECT_THROW(h.gateway->cancelOrder({ContractMetadata{}, 40, kD2}), std::invalid_argument);
    EXPECT_THROW(h.gateway->requestSnapshot({ContractMetadata{}}), std::invalid_argument);
    EXPECT_THROW(h.gateway->setHandlers(ExchangeGatewayHandlers{}), std::invalid_argument);
    EXPECT_THROW(h.gateway->poll(0), std::invalid_argument);
    EXPECT_THROW(h.gateway->poll(-1), std::invalid_argument);
    EXPECT_THROW({ BackendGateway invalid(h.server->url(), ""); (void)invalid; }, std::invalid_argument);
    h.peer.poll(subscription, 16, 20);
    EXPECT_TRUE(observed.empty());
    h.peer.close(subscription);
}

} // namespace
