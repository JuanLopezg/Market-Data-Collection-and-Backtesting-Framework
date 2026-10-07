#pragma once

#include <memory>
#include <string>

#include "message_bus.h"


// MessageBus adapter backed by NATS JetStream pull consumers
class JetStreamBus final : public MessageBus {
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

public:
    explicit JetStreamBus(const std::string& url);
    ~JetStreamBus() override;

    JetStreamBus(const JetStreamBus&) = delete;
    JetStreamBus& operator=(const JetStreamBus&) = delete;

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
