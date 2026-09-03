#pragma once

#include <mutex>
#include <unordered_map>

#include "message_bus.h"


/**************************************************************************************
 * Type    : InMemoryMessageBus
 * Purpose : Deterministic reference adapter for contract/service tests
 **************************************************************************************/
class InMemoryMessageBus final : public MessageBus {
private:
    struct Subscription {
        std::string subject;
        Handler handler;
    };

    std::mutex mutex_;
    SubscriptionID next_subscription_id_ = 1;
    std::unordered_map<SubscriptionID, Subscription> subscriptions_;

public:
    void publish(const std::string& subject, const std::string& payload) override;
    SubscriptionID subscribe(const std::string& subject, Handler handler) override;
    void unsubscribe(SubscriptionID subscriptionId) override;
    void flush() override {}
};
