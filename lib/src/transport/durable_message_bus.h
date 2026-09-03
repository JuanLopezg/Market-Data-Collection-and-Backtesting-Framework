#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "message_bus.h"


/**************************************************************************************
 * Type    : DurableMessageDisposition
 * Purpose : Transport-neutral acknowledgement decision after one durable delivery
 **************************************************************************************/
enum class DurableMessageDisposition {
    Ack,
    Retry,
    Terminate
};


/**************************************************************************************
 * Type    : DurableConsumerOptions
 * Purpose : Transport-neutral durable consumer configuration
 **************************************************************************************/
struct DurableConsumerOptions {
    std::string stream;
    std::string durable_name;
    std::string subject;
    std::int64_t ack_wait_ms = 30000;
    std::int64_t max_deliver = 10;
    std::int64_t max_ack_pending = 1024;
};


/**************************************************************************************
 * Type    : DurableMessageBus
 * Purpose : Persistence/redelivery boundary used by distributed runtime services
 *
 * Handlers return an explicit disposition. ACK is therefore emitted only after the
 * service has completed the corresponding state transition/persistence operation.
 **************************************************************************************/
class DurableMessageBus {
public:
    using SubscriptionID = std::uint64_t;
    using Handler = std::function<DurableMessageDisposition(const BusMessage&)>;

    virtual ~DurableMessageBus() = default;

    virtual void ensureStream(
        const std::string& stream,
        const std::vector<std::string>& subjects
    ) = 0;

    virtual void publish(
        const std::string& subject,
        const std::string& payload,
        const std::string& messageId
    ) = 0;

    virtual SubscriptionID subscribe(
        const DurableConsumerOptions& options,
        Handler handler
    ) = 0;

    virtual std::size_t poll(
        SubscriptionID subscriptionId,
        int maxMessages,
        std::int64_t timeoutMs
    ) = 0;

    // Release only the local binding. A durable server-side consumer must survive restart.
    virtual void close(SubscriptionID subscriptionId) = 0;
    virtual void flush() = 0;
};
