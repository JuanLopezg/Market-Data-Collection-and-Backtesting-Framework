#include "in_memory_message_bus.h"

#include <stdexcept>
#include <utility>
#include <vector>


void InMemoryMessageBus::publish(const std::string& subject, const std::string& payload)
{
    if (subject.empty())
        throw std::invalid_argument("Message subject cannot be empty");

    std::vector<Handler> handlers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        handlers.reserve(subscriptions_.size());
        for (const auto& [id, subscription] : subscriptions_) {
            (void)id;
            if (subscription.subject == subject)
                handlers.push_back(subscription.handler);
        }
    }

    const BusMessage message{subject, payload};
    for (const Handler& handler : handlers)
        handler(message);
}


MessageBus::SubscriptionID InMemoryMessageBus::subscribe(
    const std::string& subject,
    Handler handler
)
{
    if (subject.empty())
        throw std::invalid_argument("Subscription subject cannot be empty");
    if (!handler)
        throw std::invalid_argument("Subscription handler cannot be empty");

    std::lock_guard<std::mutex> lock(mutex_);
    const SubscriptionID id = next_subscription_id_++;
    subscriptions_.emplace(id, Subscription{subject, std::move(handler)});
    return id;
}


void InMemoryMessageBus::unsubscribe(SubscriptionID subscriptionId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    subscriptions_.erase(subscriptionId);
}
