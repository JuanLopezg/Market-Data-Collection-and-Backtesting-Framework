#include <memory>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "execution_messages.h"
#include "jetstream_bus.h"
#include "market_data_examples.h"
#include "message_json.h"
#include "message_subjects.h"
#include "test_support.h"
#include "update_publisher.h"

namespace market_data_test {
namespace {

// Holding a bound, non-listening socket reserves an unavailable loopback endpoint;
// no process can acquire that port between selecting it and attempting publication.
class UnavailableNatsEndpoint {
public:
    UnavailableNatsEndpoint()
    {
        socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (socket_ < 0)
            throw std::runtime_error("Cannot reserve unavailable NATS endpoint");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t size = sizeof(address);
        if (::bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ::getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
            ::close(socket_);
            throw std::runtime_error("Cannot bind unavailable NATS endpoint");
        }
        url_ = "nats://127.0.0.1:" + std::to_string(ntohs(address.sin_port));
    }
    ~UnavailableNatsEndpoint() { ::close(socket_); }
    UnavailableNatsEndpoint(const UnavailableNatsEndpoint&) = delete;
    UnavailableNatsEndpoint& operator=(const UnavailableNatsEndpoint&) = delete;
    const std::string& url() const { return url_; }

private:
    int socket_ = -1;
    std::string url_;
};

class MarketUpdatePublisherBehaviour : public testing::Test {
protected:
    test_support::TempDir temp_;
    std::unique_ptr<test_support::DockerContainer> server_;
    std::unique_ptr<JetStreamBus> reader_;

    void SetUp() override
    {
        if (!test_support::dockerAvailable()) GTEST_SKIP() << "Docker is not available";
        server_ = test_support::DockerContainer::startNats();
        reader_ = std::make_unique<JetStreamBus>(server_->url());
        reader_->ensureStream("MARKET_DATA_TEST", MessageSubjects::tradingRuntimeSubjects());
    }

    MarketDataConfig config() const
    {
        MarketDataConfig value;
        value.database_path = temp_ / "market.db";
        value.nats_url = server_->url();
        value.stream = "MARKET_DATA_TEST";
        return value;
    }

    static MarketDataIngestionSummary summary()
    {
        MarketDataIngestionSummary value;
        value.target_date = JAN2;
        value.ranked_symbols = 2;
        value.active_top_n = 1;
        value.tracked_symbols = 3;
        value.maintained_symbols = 2;
        value.requested_symbols = 2;
        value.downloaded_rows = 6;
        return value;
    }

    void commitExample()
    {
        MarketDataStore store(config().database_path);
        auto bars = barsFor("BTCUSDT", {20231231, 20240101, 20240102});
        bars.data["ETHUSDT"] = barsFor("ETHUSDT", {20231231, 20240101, 20240102}).data.at("ETHUSDT");
        store.commitDailyUpdate({JAN2, {{"BTCUSDT", 0}, {"ETHUSDT", 2}, {"OLDUSDT", 4}}},
            bars, {{"ETHUSDT", 900}, {"BTCUSDT", 731}}, JAN2);
    }

    std::vector<BusMessage> collect(std::size_t wanted)
    {
        DurableConsumerOptions options;
        options.stream = "MARKET_DATA_TEST";
        options.durable_name = "market_data_observer";
        options.subject = ">";
        std::vector<BusMessage> received;
        const auto subscription = reader_->subscribe(options, [&](const BusMessage& message) {
            received.push_back(message);
            return DurableMessageDisposition::Ack;
        });
        for (int attempt = 0; attempt < 4 && received.size() < wanted; ++attempt)
            (void)reader_->poll(subscription, 10, 500);
        reader_->flush();
        // Detect extra messages, including a producer retry that was not deduplicated.
        (void)reader_->poll(subscription, 10, 250);
        reader_->flush();
        reader_->close(subscription);
        return received;
    }
};

TEST_F(MarketUpdatePublisherBehaviour, R17_ReadinessContainsExactCountsAndAlreadyCommittedHistory)
{
    if (!test_support::dockerAvailable()) GTEST_SKIP() << "Docker is not available";
    commitExample();
    MarketDataUpdatePublisher publisher(config());
    publisher.publish(summary());
    const auto received = collect(1);
    ASSERT_EQ(received.size(), 1U);
    EXPECT_EQ(received[0].subject, "market.data.updated.v1");
    const auto event = MessageJson::decodeMarketDataUpdated(received[0].payload);
    EXPECT_EQ(event.metadata.schema_version, 1U);
    EXPECT_EQ(event.metadata.message_id, "market-data-updated:20240102");
    EXPECT_EQ(event.metadata.correlation_id, "market-data-updated:20240102");
    EXPECT_EQ(event.metadata.produced_at, 20240102U);
    EXPECT_EQ(event.completed_through, 20240102U);
    EXPECT_EQ(event.source, "binance");
    EXPECT_EQ(event.timeframe, "1d");
    EXPECT_EQ(event.ranked_symbols, 2U);
    EXPECT_EQ(event.active_top_n, 1U);
    EXPECT_EQ(event.tracked_symbols, 3U);
    EXPECT_EQ(event.maintained_symbols, 2U);
    EXPECT_EQ(event.requested_symbols, 2U);
    EXPECT_EQ(event.downloaded_rows, 6U);
    // A fresh downstream reader observes the whole completed checkpoint.
    MarketDataStore downstream(config().database_path);
    EXPECT_EQ(downstream.loadTracked().date, JAN2);
    EXPECT_EQ(downstream.countRankedFor(JAN2), 2);
    EXPECT_EQ(downstream.symbolsWithBarOn(JAN2), (std::vector<std::string>{"BTCUSDT", "ETHUSDT"}));
    EXPECT_EQ(readRows(config().database_path, "SELECT COUNT(*) FROM ohlcv_data"),
        Json::array({Json::array({6})}));
}

TEST_F(MarketUpdatePublisherBehaviour, R17_ImmediateRetryDeduplicatesButTheNextCompletedDayIsDistinct)
{
    if (!test_support::dockerAvailable()) GTEST_SKIP() << "Docker is not available";
    MarketDataUpdatePublisher publisher(config());
    publisher.publish(summary());
    // Recreate the producer as on restart after commit but before recording success.
    MarketDataUpdatePublisher restarted(config());
    restarted.publish(summary());
    auto next = summary();
    next.target_date = JAN3;
    restarted.publish(next);
    const auto received = collect(2);
    ASSERT_EQ(received.size(), 2U);
    const auto first = MessageJson::decodeMarketDataUpdated(received[0].payload);
    const auto second = MessageJson::decodeMarketDataUpdated(received[1].payload);
    EXPECT_EQ(first.completed_through, 20240102U);
    EXPECT_EQ(first.metadata.message_id, "market-data-updated:20240102");
    EXPECT_EQ(second.completed_through, 20240103U);
    EXPECT_EQ(second.metadata.message_id, "market-data-updated:20240103");
}

TEST_F(MarketUpdatePublisherBehaviour, R18_PaperPricesPrecedeReadinessCrossYearAndDeduplicateTogether)
{
    if (!test_support::dockerAvailable()) GTEST_SKIP() << "Docker is not available";
    auto value = summary();
    value.target_date = std::chrono::year{2024}/12/31;
    value.paper_opening_prices = {{"BTCUSDT", 125}, {"ETHUSDT", 250}};
    MarketDataUpdatePublisher publisher(config());
    publisher.publish(value);
    publisher.publish(value);
    const auto received = collect(2);
    ASSERT_EQ(received.size(), 2U);
    EXPECT_EQ(received[0].subject, "execution.prices.v1");
    EXPECT_EQ(received[1].subject, "market.data.updated.v1");
    const auto prices = MessageJson::decodeExecutionPriceSnapshot(received[0].payload);
    EXPECT_EQ(prices.metadata.schema_version, 1U);
    EXPECT_EQ(prices.metadata.message_id, "paper-execution-open:20241231");
    EXPECT_EQ(prices.metadata.correlation_id, "paper-execution-open:20241231");
    EXPECT_EQ(prices.metadata.produced_at, 20250101U);
    EXPECT_EQ(prices.timestamp, 20250101U);
    EXPECT_EQ(prices.decision_timestamp, 20241231U);
    EXPECT_EQ(prices.prices, (std::unordered_map<Coin, double>{{"BTCUSDT", 125}, {"ETHUSDT", 250}}));
    const auto event = MessageJson::decodeMarketDataUpdated(received[1].payload);
    EXPECT_EQ(event.completed_through, 20241231U);
    EXPECT_EQ(event.metadata.produced_at, 20241231U);
}

TEST_F(MarketUpdatePublisherBehaviour, R17_FailedPublicationLeavesSqliteCheckpointAvailableForRetry)
{
    if (!test_support::dockerAvailable()) GTEST_SKIP() << "Docker is not available";
    commitExample();
    UnavailableNatsEndpoint unavailable;
    auto settings = config();
    settings.nats_url = unavailable.url();
    MarketDataUpdatePublisher failed(settings);
    EXPECT_THROW(failed.publish(summary()), std::exception);
    MarketDataStore recovered(settings.database_path);
    EXPECT_EQ(recovered.loadTracked().date, JAN2);
    EXPECT_EQ(recovered.countRankedFor(JAN2), 2);
    EXPECT_EQ(readRows(settings.database_path, "SELECT COUNT(*) FROM ohlcv_data"),
        Json::array({Json::array({6})}));
    MarketDataUpdatePublisher retry(config());
    retry.publish(summary());
    const auto received = collect(1);
    ASSERT_EQ(received.size(), 1U);
    EXPECT_EQ(MessageJson::decodeMarketDataUpdated(received[0].payload).metadata.message_id,
        "market-data-updated:20240102");
}

} // namespace
} // namespace market_data_test
