#pragma once

#include <functional>
#include <string>

#include "exchange_gateway_adapter.h"
#include "jetstream_bus.h"


// Bridge the gateway contract to the simulated exchange backend over a
// private durable JetStream stream.
//
// The public gateway contract stays independent from the backend implementation. A real
// venue adapter can implement ExchangeGatewayAdapter without changing downstream services.
class BackendGateway final : public ExchangeGatewayAdapter {
private:
    std::string stream_;
    JetStreamBus bus_;
    ExchangeGatewayHandlers handlers_;
    std::function<bool(Timestamp)> event_time_gate_;

    MessageBus::SubscriptionID event_subscription_ = 0;
    MessageBus::SubscriptionID snapshot_subscription_ = 0;

    DurableConsumerOptions consumer(
        const std::string& durable,
        const std::string& subject
    ) const;

public:
    BackendGateway(
        const std::string& natsUrl,
        std::string stream
    );
    ~BackendGateway() override;

    void setHandlers(ExchangeGatewayHandlers handlers) override;
    void setEventTimeGate(std::function<bool(Timestamp)> gate);
    void submitOrder(const SubmitOrderCommand& command) override;
    void cancelOrder(const CancelOrderCommand& command) override;
    void requestSnapshot(const ExchangeSnapshotRequest& request) override;
    void poll(int timeoutMs) override;
};
