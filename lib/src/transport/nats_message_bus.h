#pragma once

#include <memory>
#include <string>

#include "message_bus.h"


/**************************************************************************************
 * Type    : NatsMessageBus
 * Purpose : NATS Core adapter for the transport-neutral MessageBus boundary
 *
 * This adapter deliberately exposes no NATS types to domain/runtime code. JetStream
 * durable-consumer policy is configured by the distributed service layer, not by DTOs.
 **************************************************************************************/
class NatsMessageBus final : public MessageBus {
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

public:
    explicit NatsMessageBus(const std::string& url);
    ~NatsMessageBus() override;

    NatsMessageBus(const NatsMessageBus&) = delete;
    NatsMessageBus& operator=(const NatsMessageBus&) = delete;

    void publish(const std::string& subject, const std::string& payload) override;
    SubscriptionID subscribe(const std::string& subject, Handler handler) override;
    void unsubscribe(SubscriptionID subscriptionId) override;
    void flush() override;
};
