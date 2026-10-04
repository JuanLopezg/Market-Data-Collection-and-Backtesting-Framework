/*
 * File purpose: Declares the publisher that announces a completed market-data commit to the runtime message bus.
 *
 * Keep this file focused on this responsibility. Trading decisions belong in
 * their domain component; process orchestration belongs in the service application.
 */

#pragma once

#include "market_data_config.h"
#include "market_data_ingestor.h"


// Announces a completed, durable market-data update to downstream services.
class MarketDataUpdatePublisher {
public:
    explicit MarketDataUpdatePublisher(const MarketDataConfig& config);

    void publish(const MarketDataIngestionSummary& summary) const;

private:
    std::string nats_url_;
    std::string stream_;
};
