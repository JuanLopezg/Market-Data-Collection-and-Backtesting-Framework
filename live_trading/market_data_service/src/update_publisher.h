// Declares the publisher that announces a completed market-data commit to the runtime message
// bus.

#pragma once

#include "config.h"
#include "ingestion.h"


// Announces a completed, durable market-data update to downstream services.
class MarketDataUpdatePublisher {
public:
    explicit MarketDataUpdatePublisher(const MarketDataConfig& config);

    void publish(const MarketDataIngestionSummary& summary) const;

private:
    std::string nats_url_;
    std::string stream_;
};
