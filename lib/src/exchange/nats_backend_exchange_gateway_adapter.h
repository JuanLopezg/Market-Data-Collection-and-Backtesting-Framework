#pragma once

#include <functional>
#include <string>

#include "exchange_gateway_adapter.h"
#include "nats_jetstream_message_bus.h"


/**************************************************************************************
 * Type    : NatsBackendExchangeGatewayAdapter
 * Purpose : Bridge the gateway contract to the simulated exchange backend over a
 *           private durable JetStream stream.
 *
 * The public gateway contract stays independent from the backend implementation. A real
 * venue adapter can implement ExchangeGatewayAdapter without changing downstream services.
 **************************************************************************************/
class NatsBackendExchangeGatewayAdapter final : public ExchangeGatewayAdapter {
private:
    std::string stream_;
    NatsJetStreamMessageBus bus_;
    ExchangeGatewayHandlers handlers_;
    std::function<bool(Timestamp)> event_time_gate_;

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
    void setEventTimeGate(std::function<bool(Timestamp)> gate);
    void submitOrder(const SubmitOrderCommand& command) override;
    void cancelOrder(const CancelOrderCommand& command) override;
    void requestSnapshot(const ExchangeSnapshotRequest& request) override;
    void poll(int timeoutMs) override;
};
