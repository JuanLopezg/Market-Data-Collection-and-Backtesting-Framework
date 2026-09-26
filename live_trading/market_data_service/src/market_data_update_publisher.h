#pragma once

#include "market_data_config.h"
#include "market_data_ingestor.h"


class MarketDataUpdatePublisher {
public:
    explicit MarketDataUpdatePublisher(const MarketDataConfig& config);

    void publish(const MarketDataIngestionSummary& summary) const;

private:
    std::string nats_url_;
    std::string stream_;
};
