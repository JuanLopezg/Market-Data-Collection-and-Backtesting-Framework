#pragma once

#include <cstdint>
#include <functional>
#include <string>


struct BusMessage {
    std::string subject;
    std::string payload;
};


/**************************************************************************************
 * Type    : MessageBus
 * Purpose : Transport-neutral publish/subscribe boundary for distributed runtime
 *
 * DTO serialization lives outside this interface. Domain/runtime code publishes strings
 * produced by the contract codec and does not depend on NATS types or client handles.
 **************************************************************************************/
class MessageBus {
public:
    using SubscriptionID = std::uint64_t;
    using Handler = std::function<void(const BusMessage&)>;

    virtual ~MessageBus() = default;

    virtual void publish(const std::string& subject, const std::string& payload) = 0;
    virtual SubscriptionID subscribe(const std::string& subject, Handler handler) = 0;
    virtual void unsubscribe(SubscriptionID subscriptionId) = 0;
    virtual void flush() = 0;
};
