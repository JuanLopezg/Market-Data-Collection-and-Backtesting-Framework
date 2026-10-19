#include <algorithm>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "jetstream_bus.h"
#include "message_subjects.h"
#include "test_support.h"

namespace transport_test {
namespace {

using Disposition = DurableMessageDisposition;

class JetStreamBehaviour : public testing::Test {
protected:
    std::unique_ptr<test_support::DockerContainer> server_;
    std::unique_ptr<JetStreamBus> bus_;

    void SetUp() override
    {
        if (!test_support::dockerAvailable()) GTEST_SKIP() << "Docker is not available";
        server_ = test_support::DockerContainer::startNats();
        reconnect();
    }

    void reconnect()
    {
        bus_.reset();
        bus_ = std::make_unique<JetStreamBus>(server_->url());
    }

    void prepare(const std::vector<std::string>& subjects = {MessageSubjects::FILL})
    {
        bus_->ensureStream("TRANSPORT_TEST", subjects);
    }

    static DurableConsumerOptions options(const std::string& subject = MessageSubjects::FILL)
    {
        DurableConsumerOptions consumer;
        consumer.stream = "TRANSPORT_TEST";
        consumer.durable_name = "transport_consumer";
        consumer.subject = subject;
        consumer.ack_wait_ms = 200;
        consumer.max_deliver = 5;
        consumer.max_ack_pending = 32;
        return consumer;
    }

    // Bounded real pull I/O waits for broker delivery/redelivery, without sleeps or
    // reading wall time as an economic timestamp. Callers assert final counts.
    std::size_t collect(MessageBus::SubscriptionID subscription, std::size_t wanted)
    {
        std::size_t handled = 0;
        for (int attempt = 0; attempt < 6 && handled < wanted; ++attempt)
            handled += bus_->poll(subscription, 1, 500);
        return handled;
    }

    void assertAckFloorSurvivesRebind(const std::string& subject)
    {
        prepare();
        bus_->publish(MessageSubjects::FILL, "first", "fill-101");
        std::vector<std::string> received;
        auto consumer = options(subject);
        auto subscription = bus_->subscribe(consumer, [&](const BusMessage& message) {
            received.push_back(message.payload);
            return Disposition::Ack;
        });
        ASSERT_EQ(collect(subscription, 1), 1U);
        bus_->flush();
        bus_->close(subscription);
        reconnect();
        subscription = bus_->subscribe(consumer, [&](const BusMessage& message) {
            received.push_back(message.payload);
            return Disposition::Ack;
        });
        bus_->publish(MessageSubjects::FILL, "second", "fill-102");
        ASSERT_EQ(collect(subscription, 1), 1U);
        EXPECT_EQ(received, (std::vector<std::string>{"first", "second"}));
        bus_->flush();
        EXPECT_EQ(bus_->poll(subscription, 1, 250), 0U);

        // A separate durable has an independent ACK floor and sees retained history.
        consumer.durable_name = "audit_consumer";
        std::vector<std::string> audited;
        const auto audit = bus_->subscribe(consumer, [&](const BusMessage& message) {
            audited.push_back(message.payload);
            return Disposition::Ack;
        });
        ASSERT_EQ(collect(audit, 2), 2U);
        EXPECT_EQ(audited, (std::vector<std::string>{"first", "second"}));
    }
};

TEST_F(JetStreamBehaviour, R13_PublishBeforeSubscribeRetainsExactSubjectAndBinaryPayload)
{
    if (!test_support::dockerAvailable()) GTEST_SKIP() << "Docker is not available";
    prepare();
    const std::string payload("fill\0event", 10);
    bus_->publish(MessageSubjects::FILL, payload, "binary-fill-101");
    bus_->flush();
    std::vector<BusMessage> received;
    const auto subscription = bus_->subscribe(options(), [&](const BusMessage& message) {
        received.push_back(message);
        return Disposition::Ack;
    });
    ASSERT_EQ(collect(subscription, 1), 1U);
    ASSERT_EQ(received.size(), 1U);
    EXPECT_EQ(received.front().subject, "execution.event.fill.v1");
    EXPECT_EQ(received.front().payload, payload);
    EXPECT_EQ(received.front().payload.size(), 10U);
}

TEST_F(JetStreamBehaviour, R14_StableMessageIdDeduplicatesProducerRetries)
{
    if (!test_support::dockerAvailable()) GTEST_SKIP() << "Docker is not available";
    prepare();
    bus_->publish(MessageSubjects::FILL, "Buy 2 BTC", "fill-101");
    bus_->publish(MessageSubjects::FILL, "Buy 2 BTC", "fill-101");
    bus_->publish(MessageSubjects::FILL, "Buy 2 BTC", "fill-102");
    std::vector<std::string> received;
    const auto subscription = bus_->subscribe(options(), [&](const BusMessage& message) {
        received.push_back(message.payload);
        return Disposition::Ack;
    });
    ASSERT_EQ(collect(subscription, 2), 2U);
    EXPECT_EQ(received, (std::vector<std::string>{"Buy 2 BTC", "Buy 2 BTC"}));
    bus_->flush();
    EXPECT_EQ(bus_->poll(subscription, 1, 250), 0U);
}

TEST_F(JetStreamBehaviour, R15_AckFloorSurvivesClosingExactSubjectAndRecreatingBus)
{
    if (!test_support::dockerAvailable()) GTEST_SKIP() << "Docker is not available";
    assertAckFloorSurvivesRebind(MessageSubjects::FILL);
}

TEST_F(JetStreamBehaviour, R15_AckFloorSurvivesClosingWildcardAndRecreatingBus)
{
    if (!test_support::dockerAvailable()) GTEST_SKIP() << "Docker is not available";
    assertAckFloorSurvivesRebind(MessageSubjects::EXECUTION_EVENT_FILTER);
}

TEST_F(JetStreamBehaviour, R16_RetryRedeliversTheSameMessageUntilAck)
{
    if (!test_support::dockerAvailable()) GTEST_SKIP() << "Docker is not available";
    prepare();
    bus_->publish(MessageSubjects::FILL, "fill-101 payload", "fill-101");
    std::vector<BusMessage> received;
    auto subscription = bus_->subscribe(options(), [&](const BusMessage& message) {
        received.push_back(message);
        return received.size() == 1 ? Disposition::Retry : Disposition::Ack;
    });
    ASSERT_EQ(collect(subscription, 2), 2U);
    ASSERT_EQ(received.size(), 2U);
    for (const auto& message : received) {
        EXPECT_EQ(message.subject, "execution.event.fill.v1");
        EXPECT_EQ(message.payload, "fill-101 payload");
    }
    bus_->flush();
    bus_->close(subscription);
    subscription = bus_->subscribe(options(), [&](const BusMessage& message) {
        received.push_back(message);
        return Disposition::Ack;
    });
    EXPECT_EQ(bus_->poll(subscription, 1, 250), 0U);
    EXPECT_EQ(received.size(), 2U);
}

TEST_F(JetStreamBehaviour, R17_TerminateStopsRedeliveryButPreservesLaterMessages)
{
    if (!test_support::dockerAvailable()) GTEST_SKIP() << "Docker is not available";
    prepare();
    bus_->publish(MessageSubjects::FILL, "invalid", "invalid-101");
    std::vector<std::string> received;
    auto subscription = bus_->subscribe(options(), [&](const BusMessage& message) {
        received.push_back(message.payload);
        return Disposition::Terminate;
    });
    ASSERT_EQ(collect(subscription, 1), 1U);
    bus_->flush();
    bus_->close(subscription);
    reconnect();
    subscription = bus_->subscribe(options(), [&](const BusMessage& message) {
        received.push_back(message.payload);
        return Disposition::Ack;
    });
    bus_->publish(MessageSubjects::FILL, "valid", "valid-102");
    ASSERT_EQ(collect(subscription, 1), 1U);
    EXPECT_EQ(received, (std::vector<std::string>{"invalid", "valid"}));
    bus_->flush();
    EXPECT_EQ(bus_->poll(subscription, 1, 250), 0U);
    auto auditConsumer = options();
    auditConsumer.durable_name = "termination_audit";
    std::vector<std::string> retained;
    const auto audit = bus_->subscribe(auditConsumer, [&](const BusMessage& message) {
        retained.push_back(message.payload);
        return Disposition::Ack;
    });
    ASSERT_EQ(collect(audit, 2), 2U);
    EXPECT_EQ(retained, (std::vector<std::string>{"invalid", "valid"}));
}

TEST_F(JetStreamBehaviour, R18_HandlerExceptionLeavesDeliveryUnacknowledgedAcrossRestart)
{
    if (!test_support::dockerAvailable()) GTEST_SKIP() << "Docker is not available";
    prepare();
    bus_->publish(MessageSubjects::FILL, "committed before crash", "fill-101");
    int attempts = 0;
    auto subscription = bus_->subscribe(options(), [&](const BusMessage&) -> Disposition {
        ++attempts;
        throw std::runtime_error("Injected crash after local persistence before ACK");
    });
    ASSERT_THROW((void)bus_->poll(subscription, 1, 1000), std::runtime_error);
    EXPECT_EQ(attempts, 1);
    bus_->close(subscription);
    reconnect();
    std::vector<std::string> received;
    subscription = bus_->subscribe(options(), [&](const BusMessage& message) {
        received.push_back(message.payload);
        return Disposition::Ack;
    });
    ASSERT_EQ(collect(subscription, 1), 1U);
    EXPECT_EQ(received, (std::vector<std::string>{"committed before crash"}));
    bus_->flush();
    EXPECT_EQ(bus_->poll(subscription, 1, 250), 0U);
}

TEST_F(JetStreamBehaviour, R18_ExceptionDoesNotLoseUntouchedMessagesFromFetchedBatch)
{
    if (!test_support::dockerAvailable()) GTEST_SKIP() << "Docker is not available";
    prepare();
    bus_->publish(MessageSubjects::FILL, "first", "fill-101");
    bus_->publish(MessageSubjects::FILL, "second", "fill-102");
    std::vector<std::string> attempted;
    auto subscription = bus_->subscribe(options(), [&](const BusMessage& message) -> Disposition {
        attempted.push_back(message.payload);
        throw std::runtime_error("Injected first-message failure");
    });
    ASSERT_THROW((void)bus_->poll(subscription, 2, 1000), std::runtime_error);
    EXPECT_EQ(attempted, (std::vector<std::string>{"first"}));
    bus_->close(subscription);
    reconnect();
    std::vector<std::string> recovered;
    subscription = bus_->subscribe(options(), [&](const BusMessage& message) {
        recovered.push_back(message.payload);
        return Disposition::Ack;
    });
    ASSERT_EQ(collect(subscription, 2), 2U);
    std::sort(recovered.begin(), recovered.end());
    EXPECT_EQ(recovered, (std::vector<std::string>{"first", "second"}));
    bus_->flush();
    EXPECT_EQ(bus_->poll(subscription, 1, 250), 0U);
}

TEST_F(JetStreamBehaviour, R19_EnsureStreamIsIdempotentAndAddsSubjectsWithoutDataLoss)
{
    if (!test_support::dockerAvailable()) GTEST_SKIP() << "Docker is not available";
    prepare();
    bus_->publish(MessageSubjects::FILL, "fill", "fill-101");
    for (int repeat = 0; repeat < 2; ++repeat)
        prepare({MessageSubjects::FILL, MessageSubjects::VENUE_COST});
    bus_->publish(MessageSubjects::VENUE_COST, "funding -2.5", "cost-102");
    std::map<std::string, std::string> received;
    const auto subscription = bus_->subscribe(options(MessageSubjects::EXECUTION_EVENT_FILTER), [&](const BusMessage& message) {
        received.emplace(message.subject, message.payload);
        return Disposition::Ack;
    });
    ASSERT_EQ(collect(subscription, 2), 2U);
    EXPECT_EQ(received, (std::map<std::string, std::string>{
        {"execution.event.fill.v1", "fill"}, {"execution.event.venue_cost.v1", "funding -2.5"}}));
    bus_->flush();
    EXPECT_EQ(bus_->poll(subscription, 1, 250), 0U);
}

TEST_F(JetStreamBehaviour, R19_BrokerRestartPreservesRetainedMessagesAndDurableAckFloor)
{
    if (!test_support::dockerAvailable()) GTEST_SKIP() << "Docker is not available";
    prepare();
    bus_->publish(MessageSubjects::FILL, "acked", "fill-101");
    std::vector<std::string> received;
    auto subscription = bus_->subscribe(options(), [&](const BusMessage& message) {
        received.push_back(message.payload);
        return Disposition::Ack;
    });
    ASSERT_EQ(collect(subscription, 1), 1U);
    bus_->flush();
    bus_->publish(MessageSubjects::FILL, "pending", "fill-102");
    bus_->flush();
    bus_.reset();
    server_->restart();
    reconnect();
    subscription = bus_->subscribe(options(), [&](const BusMessage& message) {
        received.push_back(message.payload);
        return Disposition::Ack;
    });
    ASSERT_EQ(collect(subscription, 1), 1U);
    EXPECT_EQ(received, (std::vector<std::string>{"acked", "pending"}));
    bus_->flush();
    EXPECT_EQ(bus_->poll(subscription, 1, 250), 0U);
}

TEST_F(JetStreamBehaviour, R20_InvalidOptionsPollArgumentsAndUnknownSubscriptionsThrow)
{
    if (!test_support::dockerAvailable()) GTEST_SKIP() << "Docker is not available";
    prepare();
    const auto ack = [](const BusMessage&) { return Disposition::Ack; };
    for (int field = 0; field < 7; ++field) {
        SCOPED_TRACE(field);
        auto bad = options();
        if (field == 0) bad.stream.clear();
        if (field == 1) bad.durable_name.clear();
        if (field == 2) bad.subject.clear();
        if (field == 3) bad.durable_name = "bad.name";
        if (field == 4) bad.ack_wait_ms = 0;
        if (field == 5) bad.max_deliver = 0;
        if (field == 6) bad.max_ack_pending = 0;
        EXPECT_THROW((void)bus_->subscribe(bad, ack), std::invalid_argument);
    }
    EXPECT_THROW((void)bus_->subscribe(options(), {}), std::invalid_argument);
    EXPECT_THROW(bus_->ensureStream("", {MessageSubjects::FILL}), std::invalid_argument);
    EXPECT_THROW(bus_->ensureStream("TRANSPORT_TEST", {}), std::invalid_argument);
    EXPECT_THROW(bus_->ensureStream("TRANSPORT_TEST", {""}), std::invalid_argument);
    EXPECT_THROW(bus_->publish("", "payload", "id"), std::invalid_argument);
    const auto subscription = bus_->subscribe(options(), ack);
    EXPECT_THROW((void)bus_->poll(subscription, 0, 100), std::invalid_argument);
    EXPECT_THROW((void)bus_->poll(subscription, 1, 0), std::invalid_argument);
    EXPECT_THROW((void)bus_->poll(subscription, -1, 100), std::invalid_argument);
    EXPECT_THROW((void)bus_->poll(subscription, 1, -1), std::invalid_argument);
    EXPECT_THROW((void)bus_->poll(999999, 1, 100), std::out_of_range);
    EXPECT_EQ(bus_->poll(subscription, 1, 100), 0U);
    EXPECT_NO_THROW(bus_->close(subscription));
    EXPECT_NO_THROW(bus_->close(subscription));
    EXPECT_THROW((void)bus_->poll(subscription, 1, 100), std::out_of_range);
}

TEST_F(JetStreamBehaviour, R20_IncompatibleDurableFilterFailsWithoutResettingProgress)
{
    if (!test_support::dockerAvailable()) GTEST_SKIP() << "Docker is not available";
    prepare({MessageSubjects::FILL, MessageSubjects::VENUE_COST});
    bus_->publish(MessageSubjects::FILL, "first", "fill-101");
    auto consumer = options();
    std::vector<std::string> received;
    auto subscription = bus_->subscribe(consumer, [&](const BusMessage& message) {
        received.push_back(message.payload);
        return Disposition::Ack;
    });
    ASSERT_EQ(collect(subscription, 1), 1U);
    bus_->flush();
    bus_->close(subscription);
    consumer.subject = MessageSubjects::VENUE_COST;
    EXPECT_THROW((void)bus_->subscribe(consumer, [](const BusMessage&) { return Disposition::Ack; }), std::runtime_error);
    consumer.subject = MessageSubjects::FILL;
    subscription = bus_->subscribe(consumer, [&](const BusMessage& message) {
        received.push_back(message.payload);
        return Disposition::Ack;
    });
    bus_->publish(MessageSubjects::FILL, "second", "fill-102");
    ASSERT_EQ(collect(subscription, 1), 1U);
    EXPECT_EQ(received, (std::vector<std::string>{"first", "second"}));
}

TEST_F(JetStreamBehaviour, R12_WildcardDeliversOnlyExecutionEvents)
{
    if (!test_support::dockerAvailable()) GTEST_SKIP() << "Docker is not available";
    prepare({MessageSubjects::EXECUTION_EVENT_FILTER, MessageSubjects::SUBMIT_ORDER});
    bus_->publish(MessageSubjects::FILL, "fill", "fill-101");
    bus_->publish(MessageSubjects::ORDER_UPDATE, "partial", "update-101");
    bus_->publish(MessageSubjects::VENUE_COST, "fee", "fee-101");
    bus_->publish(MessageSubjects::SUBMIT_ORDER, "not an event", "submit-102");
    std::map<std::string, std::string> received;
    const auto subscription = bus_->subscribe(options(MessageSubjects::EXECUTION_EVENT_FILTER), [&](const BusMessage& message) {
        received.emplace(message.subject, message.payload);
        return Disposition::Ack;
    });
    ASSERT_EQ(collect(subscription, 3), 3U);
    EXPECT_EQ(received, (std::map<std::string, std::string>{
        {"execution.event.fill.v1", "fill"}, {"execution.event.order_update.v1", "partial"},
        {"execution.event.venue_cost.v1", "fee"}}));
    bus_->flush();
    EXPECT_EQ(bus_->poll(subscription, 1, 250), 0U);
}

} // namespace
} // namespace transport_test
