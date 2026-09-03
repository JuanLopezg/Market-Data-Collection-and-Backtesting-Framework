#pragma once

#include <memory>
#include <string>

#include "durable_message_bus.h"


/**************************************************************************************
 * Type    : NatsJetStreamMessageBus
 * Purpose : DurableMessageBus adapter backed by NATS JetStream pull consumers
 **************************************************************************************/
class NatsJetStreamMessageBus final : public DurableMessageBus {
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

public:
    explicit NatsJetStreamMessageBus(const std::string& url);
    ~NatsJetStreamMessageBus() override;

    NatsJetStreamMessageBus(const NatsJetStreamMessageBus&) = delete;
    NatsJetStreamMessageBus& operator=(const NatsJetStreamMessageBus&) = delete;

    void ensureStream(
        const std::string& stream,
        const std::vector<std::string>& subjects
    ) override;

    void publish(
        const std::string& subject,
        const std::string& payload,
        const std::string& messageId
    ) override;

    SubscriptionID subscribe(
        const DurableConsumerOptions& options,
        Handler handler
    ) override;

    std::size_t poll(
        SubscriptionID subscriptionId,
        int maxMessages,
        std::int64_t timeoutMs
    ) override;

    void close(SubscriptionID subscriptionId) override;
    void flush() override;
};
