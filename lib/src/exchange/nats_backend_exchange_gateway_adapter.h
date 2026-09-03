#pragma once

#include <string>

#include "exchange_gateway_adapter.h"
#include "nats_jetstream_message_bus.h"


/**************************************************************************************
 * Type    : NatsBackendExchangeGatewayAdapter
 * Purpose : Replay/test adapter that bridges the live gateway contract to a simulated
 *           exchange service over a private durable JetStream stream
 *
 * PATCH 25 will provide the simulated-exchange service consuming the private backend
 * subjects. A future live adapter can implement ExchangeGatewayAdapter directly against
 * Binance without changing the public gateway subjects or downstream services.
 **************************************************************************************/
class NatsBackendExchangeGatewayAdapter final : public ExchangeGatewayAdapter {
private:
    std::string stream_;
    NatsJetStreamMessageBus bus_;
    ExchangeGatewayHandlers handlers_;

    DurableMessageBus::SubscriptionID event_subscription_ = 0;
    DurableMessageBus::SubscriptionID snapshot_subscription_ = 0;

    DurableConsumerOptions consumer(
        const std::string& durable,
        const std::string& subject
    ) const;

public:
    NatsBackendExchangeGatewayAdapter(
        const std::string& natsUrl,
        std::string stream
    );
    ~NatsBackendExchangeGatewayAdapter() override;

    void setHandlers(ExchangeGatewayHandlers handlers) override;
    void submitOrder(const SubmitOrderCommand& command) override;
    void cancelOrder(const CancelOrderCommand& command) override;
    void requestSnapshot(const ExchangeSnapshotRequest& request) override;
    void poll(int timeoutMs) override;
};
