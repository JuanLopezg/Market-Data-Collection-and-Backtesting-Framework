#include "nats_message_bus.h"

#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include <nats/nats.h>


namespace {

void checkNats(natsStatus status, const char* context)
{
    if (status == NATS_OK)
        return;

    throw std::runtime_error(
        std::string(context) + ": " + natsStatus_GetText(status)
    );
}

}


struct NatsMessageBus::Impl {
    struct SubscriptionEntry {
        Impl* owner = nullptr;
        SubscriptionID id = 0;
        Handler handler;
        natsSubscription* subscription = nullptr;
    };

    natsConnection* connection = nullptr;
    std::mutex mutex;
    SubscriptionID next_subscription_id = 1;
    std::unordered_map<SubscriptionID, std::unique_ptr<SubscriptionEntry>> subscriptions;

    explicit Impl(const std::string& url)
    {
        checkNats(
            natsConnection_ConnectTo(&connection, url.c_str()),
            "Failed to connect to NATS"
        );
    }

    ~Impl()
    {
        for (auto& [id, entry] : subscriptions) {
            (void)id;
            if (entry->subscription)
                natsSubscription_Destroy(entry->subscription);
        }
        subscriptions.clear();

        if (connection)
            natsConnection_Destroy(connection);
    }

    static void onMessage(
        natsConnection* connection,
        natsSubscription* subscription,
        natsMsg* message,
        void* closure
    )
    {
        (void)connection;
        (void)subscription;

        auto* entry = static_cast<SubscriptionEntry*>(closure);
        Handler handler;
        {
            std::lock_guard<std::mutex> lock(entry->owner->mutex);
            const auto it = entry->owner->subscriptions.find(entry->id);
            if (it != entry->owner->subscriptions.end())
                handler = it->second->handler;
        }

        if (handler) {
            const char* subject = natsMsg_GetSubject(message);
            const char* data = natsMsg_GetData(message);
            const int length = natsMsg_GetDataLength(message);

            handler(BusMessage{
                subject ? subject : "",
                data && length > 0 ? std::string(data, static_cast<std::size_t>(length)) : std::string{}
            });
        }

        natsMsg_Destroy(message);
    }
};


NatsMessageBus::NatsMessageBus(const std::string& url)
    : impl_(std::make_unique<Impl>(url))
{}


NatsMessageBus::~NatsMessageBus() = default;


void NatsMessageBus::publish(const std::string& subject, const std::string& payload)
{
    if (subject.empty())
        throw std::invalid_argument("Message subject cannot be empty");

    checkNats(
        natsConnection_Publish(
            impl_->connection,
            subject.c_str(),
            payload.data(),
            static_cast<int>(payload.size())
        ),
        "NATS publish failed"
    );
}


MessageBus::SubscriptionID NatsMessageBus::subscribe(
    const std::string& subject,
    Handler handler
)
{
    if (subject.empty())
        throw std::invalid_argument("Subscription subject cannot be empty");
    if (!handler)
        throw std::invalid_argument("Subscription handler cannot be empty");

    std::lock_guard<std::mutex> lock(impl_->mutex);

    const SubscriptionID id = impl_->next_subscription_id++;
    auto entry = std::make_unique<Impl::SubscriptionEntry>();
    entry->owner = impl_.get();
    entry->id = id;
    entry->handler = std::move(handler);

    checkNats(
        natsConnection_Subscribe(
            &entry->subscription,
            impl_->connection,
            subject.c_str(),
            &Impl::onMessage,
            entry.get()
        ),
        "NATS subscribe failed"
    );

    impl_->subscriptions.emplace(id, std::move(entry));
    return id;
}


void NatsMessageBus::unsubscribe(SubscriptionID subscriptionId)
{
    std::unique_ptr<Impl::SubscriptionEntry> entry;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto it = impl_->subscriptions.find(subscriptionId);
        if (it == impl_->subscriptions.end())
            return;

        entry = std::move(it->second);
        impl_->subscriptions.erase(it);
    }

    if (entry->subscription)
        natsSubscription_Destroy(entry->subscription);
}


void NatsMessageBus::flush()
{
    checkNats(natsConnection_Flush(impl_->connection), "NATS flush failed");
}
