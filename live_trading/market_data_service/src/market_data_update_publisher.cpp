#include "market_data_update_publisher.h"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

#include "contract_json_codec.h"
#include "market_data_updated.h"
#include "nats_jetstream_message_bus.h"
#include "service_logging.h"
#include "time_utils.h"
#include "transport_subjects.h"

namespace {

std::uint64_t countToWire(std::size_t value)
{
    return static_cast<std::uint64_t>(value);
}

std::string messageId(Timestamp completedThrough)
{
    return "market-data-updated:" + std::to_string(completedThrough);
}

} // namespace

MarketDataUpdatePublisher::MarketDataUpdatePublisher(const MarketDataConfig& config)
    : nats_url_(config.nats_url),
      stream_(config.stream)
{}

void MarketDataUpdatePublisher::publish(const MarketDataIngestionSummary& summary) const
{
    const int compactDate = toYYYYMMDD(summary.target_date);
    if (compactDate <= 0 ||
        static_cast<unsigned long long>(compactDate) >
            static_cast<unsigned long long>(std::numeric_limits<Timestamp>::max()))
        throw std::runtime_error("Market-data update date cannot be represented as Timestamp");

    const Timestamp completedThrough = static_cast<Timestamp>(compactDate);

    MarketDataUpdated event;
    event.metadata.schema_version = 1;
    event.metadata.message_id = messageId(completedThrough);
    event.metadata.correlation_id = event.metadata.message_id;
    event.metadata.produced_at = completedThrough;
    event.completed_through = completedThrough;
    event.source = "binance";
    event.timeframe = "1d";
    event.ranked_symbols = countToWire(summary.ranked_symbols);
    event.active_top_n = countToWire(summary.active_top_n);
    event.tracked_symbols = countToWire(summary.tracked_symbols);
    event.maintained_symbols = countToWire(summary.maintained_symbols);
    event.requested_symbols = countToWire(summary.requested_symbols);
    event.downloaded_rows = countToWire(summary.downloaded_rows);

    // Deliberately connect to NATS only after SQLite has committed. If NATS is down,
    // the canonical market database still contains the completed update. The scheduler
    // will retry this target date, which is safe because SQLite writes and this message
    // ID are deterministic/idempotent.
    NatsJetStreamMessageBus bus(nats_url_);
    bus.ensureStream(stream_, TransportSubjects::tradingRuntimeSubjects());

    const std::string payload = ContractJsonCodec::encode(event);
    bus.publish(
        TransportSubjects::MARKET_DATA_UPDATED,
        payload,
        event.metadata.message_id
    );
    bus.flush();

    LG_INFO(
        "service=market-data event=market_data_updated_published completed_through={} subject={} message_id={} stream={} downloaded_rows={} maintained_symbols={}",
        completedThrough,
        TransportSubjects::MARKET_DATA_UPDATED,
        event.metadata.message_id,
        stream_,
        event.downloaded_rows,
        event.maintained_symbols
    );
}
