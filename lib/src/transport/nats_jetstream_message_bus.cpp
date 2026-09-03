#include "nats_jetstream_message_bus.h"

#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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

void validateConsumerOptions(const DurableConsumerOptions& options)
{
    if (options.stream.empty())
        throw std::invalid_argument("Durable consumer stream cannot be empty");
    if (options.durable_name.empty())
        throw std::invalid_argument("Durable consumer name cannot be empty");
    if (options.durable_name.find('.') != std::string::npos)
        throw std::invalid_argument("Durable consumer name cannot contain '.'");
    if (options.subject.empty())
        throw std::invalid_argument("Durable consumer subject cannot be empty");
    if (options.ack_wait_ms <= 0)
        throw std::invalid_argument("Durable consumer ack_wait_ms must be positive");
    if (options.max_deliver <= 0)
        throw std::invalid_argument("Durable consumer max_deliver must be positive");
    if (options.max_ack_pending <= 0)
        throw std::invalid_argument("Durable consumer max_ack_pending must be positive");
}

} // namespace


struct NatsJetStreamMessageBus::Impl {
    struct SubscriptionEntry {
        Handler handler;
        natsSubscription* subscription = nullptr;
    };

    natsConnection* connection = nullptr;
    jsCtx* jetstream = nullptr;
    SubscriptionID next_subscription_id = 1;
    std::unordered_map<SubscriptionID, SubscriptionEntry> subscriptions;

    explicit Impl(const std::string& url)
    {
        checkNats(
            natsConnection_ConnectTo(&connection, url.c_str()),
            "Failed to connect to NATS"
        );

        const natsStatus status = natsConnection_JetStream(&jetstream, connection, nullptr);
        if (status != NATS_OK) {
            natsConnection_Destroy(connection);
            connection = nullptr;
            checkNats(status, "Failed to create JetStream context");
        }
    }

    ~Impl()
    {
        // Destroying the local binding (without Unsubscribe/Drain) preserves durable
        // consumers on nats.c >= 3.4, which is the minimum version used by this project.
        for (auto& [id, entry] : subscriptions) {
            (void)id;
            if (entry.subscription)
                natsSubscription_Destroy(entry.subscription);
        }
        subscriptions.clear();

        if (jetstream)
            jsCtx_Destroy(jetstream);
        if (connection)
            natsConnection_Destroy(connection);
    }
};


NatsJetStreamMessageBus::NatsJetStreamMessageBus(const std::string& url)
    : impl_(std::make_unique<Impl>(url))
{
    if (url.empty())
        throw std::invalid_argument("NATS URL cannot be empty");
}


NatsJetStreamMessageBus::~NatsJetStreamMessageBus() = default;


void NatsJetStreamMessageBus::ensureStream(
    const std::string& stream,
    const std::vector<std::string>& subjects
)
{
    if (stream.empty())
        throw std::invalid_argument("JetStream stream name cannot be empty");
    if (subjects.empty())
        throw std::invalid_argument("JetStream stream subjects cannot be empty");

    jsStreamInfo* info = nullptr;
    jsErrCode errorCode = static_cast<jsErrCode>(0);
    natsStatus status = js_GetStreamInfo(
        &info,
        impl_->jetstream,
        stream.c_str(),
        nullptr,
        &errorCode
    );

    if (status == NATS_OK) {
        if (info == nullptr || info->Config == nullptr) {
            jsStreamInfo_Destroy(info);
            throw std::runtime_error("JetStream returned stream info without config");
        }

        std::vector<std::string> mergedSubjects;
        std::unordered_set<std::string> seen;

        for (int i = 0; i < info->Config->SubjectsLen; ++i) {
            const char* existing = info->Config->Subjects[i];
            if (existing == nullptr || *existing == '\0')
                continue;
            if (seen.insert(existing).second)
                mergedSubjects.emplace_back(existing);
        }

        bool changed = false;
        for (const std::string& subject : subjects) {
            if (subject.empty()) {
                jsStreamInfo_Destroy(info);
                throw std::invalid_argument("JetStream stream subject cannot be empty");
            }
            if (seen.insert(subject).second) {
                mergedSubjects.push_back(subject);
                changed = true;
            }
        }

        if (!changed) {
            jsStreamInfo_Destroy(info);
            return;
        }

        std::vector<const char*> subjectPointers;
        subjectPointers.reserve(mergedSubjects.size());
        for (const std::string& subject : mergedSubjects)
            subjectPointers.push_back(subject.c_str());

        // Never mutate info->Config: it and all of its nested allocations are
        // owned by libnats and released by jsStreamInfo_Destroy(info). Pointing
        // its Subjects field at our std::string storage would make libnats free
        // memory it does not own. Use a shallow config copy for the synchronous
        // update call and override only the subject array in that copy.
        jsStreamConfig updatedConfig = *info->Config;
        updatedConfig.Subjects = subjectPointers.data();
        updatedConfig.SubjectsLen = static_cast<int>(subjectPointers.size());

        jsStreamInfo* updatedInfo = nullptr;
        status = js_UpdateStream(
            &updatedInfo,
            impl_->jetstream,
            &updatedConfig,
            nullptr,
            &errorCode
        );

        jsStreamInfo_Destroy(info);
        if (status != NATS_OK) {
            jsStreamInfo_Destroy(updatedInfo);
            checkNats(status, "Failed to update JetStream stream subjects");
        }

        jsStreamInfo_Destroy(updatedInfo);
        return;
    }

    if (status != NATS_NOT_FOUND)
        checkNats(status, "Failed to query JetStream stream");

    std::vector<const char*> subjectPointers;
    subjectPointers.reserve(subjects.size());
    for (const std::string& subject : subjects) {
        if (subject.empty())
            throw std::invalid_argument("JetStream stream subject cannot be empty");
        subjectPointers.push_back(subject.c_str());
    }

    jsStreamConfig config;
    checkNats(jsStreamConfig_Init(&config), "Failed to initialize JetStream stream config");
    config.Name = stream.c_str();
    config.Subjects = subjectPointers.data();
    config.SubjectsLen = static_cast<int>(subjectPointers.size());
    config.Storage = js_FileStorage;

    status = js_AddStream(
        &info,
        impl_->jetstream,
        &config,
        nullptr,
        &errorCode
    );
    if (status != NATS_OK)
        checkNats(status, "Failed to create JetStream stream");

    jsStreamInfo_Destroy(info);
}


void NatsJetStreamMessageBus::publish(
    const std::string& subject,
    const std::string& payload,
    const std::string& messageId
)
{
    if (subject.empty())
        throw std::invalid_argument("JetStream publish subject cannot be empty");

    jsPubOptions options;
    checkNats(jsPubOptions_Init(&options), "Failed to initialize JetStream publish options");
    if (!messageId.empty())
        options.MsgId = messageId.c_str();

    jsPubAck* ack = nullptr;
    jsErrCode errorCode = static_cast<jsErrCode>(0);
    const natsStatus status = js_Publish(
        &ack,
        impl_->jetstream,
        subject.c_str(),
        payload.data(),
        static_cast<int>(payload.size()),
        &options,
        &errorCode
    );

    if (status != NATS_OK) {
        jsPubAck_Destroy(ack);
        checkNats(status, "JetStream publish failed");
    }

    jsPubAck_Destroy(ack);
}


DurableMessageBus::SubscriptionID NatsJetStreamMessageBus::subscribe(
    const DurableConsumerOptions& options,
    Handler handler
)
{
    validateConsumerOptions(options);
    if (!handler)
        throw std::invalid_argument("Durable consumer handler cannot be empty");

    jsSubOptions subscribeOptions;
    checkNats(
        jsSubOptions_Init(&subscribeOptions),
        "Failed to initialize JetStream subscription options"
    );

    subscribeOptions.Stream = options.stream.c_str();
    subscribeOptions.Config.AckPolicy = js_AckExplicit;
    subscribeOptions.Config.AckWait = options.ack_wait_ms * 1000000LL;
    subscribeOptions.Config.MaxDeliver = options.max_deliver;
    subscribeOptions.Config.MaxAckPending = options.max_ack_pending;

    natsSubscription* subscription = nullptr;
    jsErrCode errorCode = static_cast<jsErrCode>(0);
    const natsStatus status = js_PullSubscribe(
        &subscription,
        impl_->jetstream,
        options.subject.c_str(),
        options.durable_name.c_str(),
        nullptr,
        &subscribeOptions,
        &errorCode
    );
    if (status != NATS_OK)
        checkNats(status, "Failed to create JetStream durable pull subscription");

    const SubscriptionID id = impl_->next_subscription_id++;
    impl_->subscriptions.emplace(
        id,
        Impl::SubscriptionEntry{std::move(handler), subscription}
    );
    return id;
}


std::size_t NatsJetStreamMessageBus::poll(
    SubscriptionID subscriptionId,
    int maxMessages,
    std::int64_t timeoutMs
)
{
    if (maxMessages <= 0)
        throw std::invalid_argument("JetStream poll batch size must be positive");
    if (timeoutMs <= 0)
        throw std::invalid_argument("JetStream poll timeout must be positive");

    const auto it = impl_->subscriptions.find(subscriptionId);
    if (it == impl_->subscriptions.end())
        throw std::out_of_range("Unknown JetStream durable subscription id");

    natsMsgList messages{};
    jsErrCode errorCode = static_cast<jsErrCode>(0);
    const natsStatus fetchStatus = natsSubscription_Fetch(
        &messages,
        it->second.subscription,
        maxMessages,
        timeoutMs,
        &errorCode
    );

    if (fetchStatus == NATS_TIMEOUT || fetchStatus == NATS_NOT_FOUND)
        return 0;
    if (fetchStatus != NATS_OK)
        checkNats(fetchStatus, "JetStream fetch failed");

    std::size_t processed = 0;
    try {
        for (int index = 0; index < messages.Count; ++index) {
            natsMsg* message = messages.Msgs[index];
            const char* subject = natsMsg_GetSubject(message);
            const char* data = natsMsg_GetData(message);
            const int length = natsMsg_GetDataLength(message);

            const DurableMessageDisposition disposition = it->second.handler(BusMessage{
                subject ? subject : "",
                data && length > 0
                    ? std::string(data, static_cast<std::size_t>(length))
                    : std::string{}
            });

            natsStatus ackStatus = NATS_OK;
            switch (disposition) {
            case DurableMessageDisposition::Ack:
                ackStatus = natsMsg_Ack(message, nullptr);
                break;
            case DurableMessageDisposition::Retry:
                ackStatus = natsMsg_Nak(message, nullptr);
                break;
            case DurableMessageDisposition::Terminate:
                ackStatus = natsMsg_Term(message, nullptr);
                break;
            }

            checkNats(ackStatus, "JetStream message acknowledgement failed");
            ++processed;
        }
    }
    catch (...) {
        natsMsgList_Destroy(&messages);
        throw;
    }

    natsMsgList_Destroy(&messages);
    return processed;
}


void NatsJetStreamMessageBus::close(SubscriptionID subscriptionId)
{
    const auto it = impl_->subscriptions.find(subscriptionId);
    if (it == impl_->subscriptions.end())
        return;

    if (it->second.subscription)
        natsSubscription_Destroy(it->second.subscription);
    impl_->subscriptions.erase(it);
}


void NatsJetStreamMessageBus::flush()
{
    checkNats(natsConnection_Flush(impl_->connection), "NATS flush failed");
}
