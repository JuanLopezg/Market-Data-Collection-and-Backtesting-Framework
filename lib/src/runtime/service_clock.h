#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "clock.h"
#include "clock_state.h"
#include "clock_sync_request.h"
#include "contract_json_codec.h"
#include "durable_message_bus.h"
#include "runtime_mode.h"
#include "service_logging.h"
#include "transport_subjects.h"


/**************************************************************************************
 * Purpose : Convert the current project's daily YYYYMMDD trading Timestamp into the
 *           common Clock time_point. This is deliberately isolated from transport and
 *           from LIVE wall-clock semantics.
 **************************************************************************************/
inline std::chrono::system_clock::time_point replayTimestampToTimePoint(Timestamp value)
{
    if (value == 0)
        throw std::invalid_argument("Replay logical timestamp cannot be zero");

    const int yearValue = static_cast<int>(value / 10000U);
    const unsigned monthValue = static_cast<unsigned>((value / 100U) % 100U);
    const unsigned dayValue = static_cast<unsigned>(value % 100U);

    const std::chrono::year_month_day date{
        std::chrono::year{yearValue},
        std::chrono::month{monthValue},
        std::chrono::day{dayValue}
    };
    if (!date.ok())
        throw std::invalid_argument("Replay logical timestamp is not a valid YYYYMMDD date");

    return std::chrono::sys_days{date};
}


/**************************************************************************************
 * Type    : ServiceClockContext
 * Purpose : Bootstrap/wiring boundary shared by distributed services.
 *
 * LIVE/TESTNET:
 *   - owns only SystemClock;
 *   - creates no ClockState consumer;
 *   - publishes no clock synchronization request;
 *   - authorize() is a no-op because the production event path is not gated by fake time.
 *
 * REPLAY:
 *   - owns SimulatedClock;
 *   - follows simulation.clock.state.v1 through a service-specific durable consumer;
 *   - requests an authority re-publish on startup, which repairs local clock state after
 *     a hard follower restart even when its durable consumer already ACKed older ticks;
 *   - authorizes a business message only when logical_time >= message event time.
 **************************************************************************************/
class ServiceClockContext final {
public:
    struct Options {
        RuntimeMode runtime_mode = RuntimeMode::Live;
        std::string stream;
        std::string service_id;
        std::string expected_simulation_id;
    };

private:
    Options options_;
    DurableMessageBus& bus_;
    SystemClock system_clock_;
    std::unique_ptr<SimulatedClock> simulated_clock_;
    std::optional<ClockState> state_;
    DurableMessageBus::SubscriptionID clock_subscription_ = 0;
    std::chrono::steady_clock::time_point next_sync_request_at_{};
    std::unordered_set<std::string> sync_request_ids_;
    bool authoritative_confirmation_observed_ = false;

    static constexpr auto SYNC_REQUEST_RETRY_INTERVAL = std::chrono::seconds(1);
    static constexpr auto STARTUP_SYNC_TIMEOUT = std::chrono::seconds(120);

    static DurableConsumerOptions clockConsumer(
        const std::string& stream,
        const std::string& serviceId
    )
    {
        DurableConsumerOptions result;
        result.stream = stream;
        result.durable_name = serviceId + "-clock-state";
        result.subject = TransportSubjects::CLOCK_STATE;
        result.ack_wait_ms = 5000; // idempotent clock control-plane: fast dead-client redelivery
        result.max_deliver = 50;
        result.max_ack_pending = 64;
        return result;
    }

    static std::string requestIdentitySuffix()
    {
        // Never use std::random_device on the restart-critical bootstrap path.  A
        // container can be alive while entropy acquisition is delayed, which would
        // postpone the very ClockSyncRequest needed to make the process operational.
        // steady_clock is technical/process time only; combined with a process-local
        // counter it gives a non-blocking transport identity without entering business
        // time semantics.  The service id is already part of the message id.
        static std::atomic<std::uint64_t> sequence{0};
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::uint64_t ordinal = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        return std::to_string(static_cast<std::uint64_t>(ticks)) + "-" +
               std::to_string(ordinal);
    }

    static bool sameSemanticState(const ClockState& lhs, const ClockState& rhs)
    {
        return lhs.simulation_id == rhs.simulation_id &&
               lhs.logical_time == rhs.logical_time &&
               lhs.revision == rhs.revision &&
               lhs.mode == rhs.mode &&
               lhs.speed_multiplier == rhs.speed_multiplier &&
               lhs.paused == rhs.paused;
    }

    DurableMessageDisposition onClockState(const BusMessage& message)
    {
        try {
            const ClockState value = ContractJsonCodec::decodeClockState(message.payload);
            if (value.metadata.schema_version != 1 || value.metadata.message_id.empty())
                return DurableMessageDisposition::Terminate;

            // A hard-restart bootstrap is complete only after this process has observed
            // a fresh authority snapshot: either a response correlated to one of THIS
            // process' sync requests or the replay-controller's periodic same-revision
            // refresh.  Merely binding an old durable and seeing an older redelivery is
            // not enough to declare the control plane ready.
            const bool correlatedSyncResponse =
                !value.metadata.correlation_id.empty() &&
                sync_request_ids_.find(value.metadata.correlation_id) != sync_request_ids_.end();
            const bool authorityRefresh =
                value.metadata.correlation_id == "replay-controller-refresh";
            if (correlatedSyncResponse || authorityRefresh)
                authoritative_confirmation_observed_ = true;

            if (!options_.expected_simulation_id.empty() &&
                value.simulation_id != options_.expected_simulation_id) {
                LG_ALERT(
                    "service={} event=clock_state_rejected reason=simulation_id_mismatch expected={} actual={} revision={}",
                    options_.service_id,
                    options_.expected_simulation_id,
                    value.simulation_id,
                    value.revision
                );
                return DurableMessageDisposition::Terminate;
            }

            if (state_ && value.simulation_id != state_->simulation_id) {
                LG_ALERT(
                    "service={} event=clock_state_rejected reason=simulation_changed active={} incoming={} revision={}",
                    options_.service_id,
                    state_->simulation_id,
                    value.simulation_id,
                    value.revision
                );
                return DurableMessageDisposition::Terminate;
            }

            if (state_ && value.revision == state_->revision && !sameSemanticState(value, *state_)) {
                LG_ALERT(
                    "service={} event=clock_state_rejected reason=same_revision_conflict simulation_id={} revision={}",
                    options_.service_id,
                    value.simulation_id,
                    value.revision
                );
                return DurableMessageDisposition::Terminate;
            }

            if (state_ && value.revision < state_->revision) {
                // A stale transport delivery has no economic meaning. Terminating the stale
                // message keeps the local monotonic clock unchanged and prevents poison loops.
                LG_WARN(
                    "service={} event=clock_state_stale disposition=terminate simulation_id={} incoming_revision={} current_revision={}",
                    options_.service_id,
                    value.simulation_id,
                    value.revision,
                    state_->revision
                );
                return DurableMessageDisposition::Terminate;
            }

            const bool firstSynchronization = !state_.has_value();
            simulated_clock_->synchronize(
                replayTimestampToTimePoint(value.logical_time),
                value.revision
            );

            if (!state_ || value.revision > state_->revision)
                state_ = value;

            if (firstSynchronization) {
                LG_INFO(
                    "service={} event=clock_synchronized simulation_id={} logical_time={} revision={} paused={}",
                    options_.service_id,
                    value.simulation_id,
                    value.logical_time,
                    value.revision,
                    value.paused ? "true" : "false"
                );
                std::cout.flush();
            }
            else {
                LG_DEBUG(
                    "service={} event=clock_state_applied simulation_id={} logical_time={} revision={} paused={}",
                    options_.service_id,
                    value.simulation_id,
                    value.logical_time,
                    value.revision,
                    value.paused ? "true" : "false"
                );
            }
            return DurableMessageDisposition::Ack;
        }
        catch (const std::invalid_argument& error) {
            LG_WARN(
                "service={} event=clock_state_invalid disposition=terminate error={}",
                options_.service_id,
                error.what()
            );
            return DurableMessageDisposition::Terminate;
        }
        catch (const std::exception& error) {
            LG_ERROR(
                "service={} event=clock_state_apply_failed disposition=retry error={}",
                options_.service_id,
                error.what()
            );
            return DurableMessageDisposition::Retry;
        }
    }

    void requestSynchronization()
    {
        ClockSyncRequest request;
        request.metadata.schema_version = 1;
        request.metadata.message_id =
            "clock-sync-request:" + options_.service_id + ":" + requestIdentitySuffix();
        request.metadata.correlation_id = options_.service_id;
        request.metadata.produced_at = state_ ? state_->logical_time : 0;
        request.requester_id = options_.service_id;
        request.simulation_id = state_ ? state_->simulation_id : options_.expected_simulation_id;
        request.known_revision = state_ ? state_->revision : 0;

        LG_INFO(
            "service={} event=clock_sync_request_attempt expected_simulation_id={} known_revision={} message_id={}",
            options_.service_id,
            request.simulation_id.empty() ? "<active>" : request.simulation_id,
            request.known_revision,
            request.metadata.message_id
        );

        std::cout.flush();

        // Keep the request identity before publish. If publish succeeds but the process
        // is interrupted before the post-publish audit line, a later correlated response
        // is still recognized as belonging to this process instance.
        sync_request_ids_.insert(request.metadata.message_id);
        bus_.publish(
            TransportSubjects::CLOCK_SYNC_REQUEST,
            ContractJsonCodec::encode(request),
            request.metadata.message_id
        );
        next_sync_request_at_ = std::chrono::steady_clock::now() + SYNC_REQUEST_RETRY_INTERVAL;
        LG_INFO(
            "service={} event=clock_sync_requested expected_simulation_id={} known_revision={} message_id={}",
            options_.service_id,
            request.simulation_id.empty() ? "<active>" : request.simulation_id,
            request.known_revision,
            request.metadata.message_id
        );
        std::cout.flush();
    }

    void requestSynchronizationBestEffort(const char* reason)
    {
        try {
            requestSynchronization();
        }
        catch (const std::exception& error) {
            // This retry cadence is technical/process time only. A transient transport
            // failure must not let a REPLAY follower silently remain unsynchronized forever,
            // and it must not become part of trading/event-time semantics.
            next_sync_request_at_ =
                std::chrono::steady_clock::now() + SYNC_REQUEST_RETRY_INTERVAL;
            LG_WARN(
                "service={} event=clock_sync_request_failed reason={} retry_in_ms=1000 error={}",
                options_.service_id,
                reason,
                error.what()
            );
        }
    }

    void synchronizeReplayBootstrap()
    {
        const auto deadline = std::chrono::steady_clock::now() + STARTUP_SYNC_TIMEOUT;
        LG_INFO(
            "service={} event=clock_bootstrap_wait_started timeout_ms=120000 expected_simulation_id={}",
            options_.service_id,
            options_.expected_simulation_id.empty() ? "<active>" : options_.expected_simulation_id
        );
        std::cout.flush();

        // Do not begin CSV loading, PostgreSQL recovery, adapter construction or any
        // business subscription until the REPLAY control plane has installed a fresh
        // authority snapshot for THIS process instance. This also makes restart recovery
        // independent of when the service's outer run() loop eventually starts polling.
        while (!(synchronized() && authoritative_confirmation_observed_)) {
            bus_.poll(clock_subscription_, 32, 50);

            const auto now = std::chrono::steady_clock::now();
            if (!(synchronized() && authoritative_confirmation_observed_) &&
                now >= next_sync_request_at_) {
                requestSynchronizationBestEffort("bootstrap_wait");
            }

            if (now >= deadline) {
                throw std::runtime_error(
                    "Timed out waiting for authoritative REPLAY ClockState bootstrap"
                );
            }
        }

        LG_INFO(
            "service={} event=clock_bootstrap_ready simulation_id={} logical_time={} revision={} paused={}",
            options_.service_id,
            state_->simulation_id,
            state_->logical_time,
            state_->revision,
            state_->paused ? "true" : "false"
        );
        std::cout.flush();
    }

public:
    ServiceClockContext(Options options, DurableMessageBus& bus)
        : options_(std::move(options)),
          bus_(bus)
    {
        if (options_.stream.empty() || options_.service_id.empty())
            throw std::invalid_argument("ServiceClockContext stream/service_id cannot be empty");

        if (options_.runtime_mode != RuntimeMode::Replay)
            return;

        simulated_clock_ = std::make_unique<SimulatedClock>();

        // Request the authoritative snapshot BEFORE binding the local pull subscription.
        // This ordering is intentional for hard-restart recovery.  After SIGKILL the
        // server-side durable clock consumer may still be releasing its previous local
        // binding for a short period.  If subscribe() is attempted first, a restarted
        // process can spend that interval inside the bind path without ever publishing
        // its synchronization request.  Publishing first is safe because the request is
        // itself durable and, when the durable clock consumer already exists, the
        // authority response is queued server-side until this process re-binds.  On a
        // brand-new replay, an early request can be deferred by the authority and the
        // normal first ClockState is published after the subscription is installed.
        requestSynchronizationBestEffort("startup");

        clock_subscription_ = bus_.subscribe(
            clockConsumer(options_.stream, options_.service_id),
            [this](const BusMessage& message) { return onClockState(message); }
        );

        synchronizeReplayBootstrap();
    }

    ~ServiceClockContext()
    {
        if (clock_subscription_ != 0)
            bus_.close(clock_subscription_);
    }

    ServiceClockContext(const ServiceClockContext&) = delete;
    ServiceClockContext& operator=(const ServiceClockContext&) = delete;

    Clock& clock()
    {
        if (simulated_clock_)
            return *simulated_clock_;
        return system_clock_;
    }

    const Clock& clock() const
    {
        if (simulated_clock_)
            return *simulated_clock_;
        return system_clock_;
    }

    RuntimeMode runtimeMode() const { return options_.runtime_mode; }
    bool replay() const { return options_.runtime_mode == RuntimeMode::Replay; }

    bool synchronized() const
    {
        return !replay() || (state_.has_value() && simulated_clock_->initialized());
    }

    std::optional<ClockState> state() const { return state_; }

    bool authorize(Timestamp businessTime) const
    {
        if (!replay())
            return true;
        if (!state_ || !simulated_clock_->initialized())
            return false;
        if (state_->paused)
            return false;
        // produced_at==0 is reserved by existing bootstrap/control contracts (for example
        // the initial exchange snapshot request before any execution timestamp exists).
        // It is allowed only after REPLAY clock synchronization, never before it.
        if (businessTime == 0)
            return true;
        return state_->logical_time >= businessTime;
    }

    DurableMessageDisposition gate(Timestamp businessTime, const std::string& eventName) const
    {
        if (authorize(businessTime))
            return DurableMessageDisposition::Ack;

        LG_DEBUG(
            "service={} event=clock_gate_waiting business_event={} required_time={} synchronized={} logical_time={} revision={} paused={} disposition=retry",
            options_.service_id,
            eventName,
            businessTime,
            synchronized() ? "true" : "false",
            state_ ? state_->logical_time : 0,
            state_ ? state_->revision : 0,
            state_ && state_->paused ? "true" : "false"
        );
        return DurableMessageDisposition::Retry;
    }

    DurableMessageDisposition gateMessage(
        const BusMessage& message,
        const std::string& eventName
    ) const
    {
        if (!replay())
            return DurableMessageDisposition::Ack;

        // Every current service-boundary JSON contract carries metadata.produced_at. If
        // a malformed payload cannot expose it, preserve the existing downstream decoder's
        // responsibility for deciding Terminate/Retry rather than changing error semantics here.
        try {
            const nlohmann::json value = nlohmann::json::parse(message.payload);
            if (!value.contains("metadata") || !value.at("metadata").contains("produced_at"))
                return DurableMessageDisposition::Ack;
            const Timestamp businessTime =
                value.at("metadata").at("produced_at").get<Timestamp>();
            return gate(businessTime, eventName);
        }
        catch (const std::exception&) {
            return DurableMessageDisposition::Ack;
        }
    }

    DurableMessageBus::Handler guard(
        std::string eventName,
        DurableMessageBus::Handler downstream
    )
    {
        if (!downstream)
            throw std::invalid_argument("Clock-guard downstream handler cannot be empty");

        return [this, eventName = std::move(eventName), downstream = std::move(downstream)](
            const BusMessage& message
        ) {
            const DurableMessageDisposition clockDisposition = gateMessage(message, eventName);
            if (clockDisposition != DurableMessageDisposition::Ack)
                return clockDisposition;
            return downstream(message);
        };
    }

    void poll(int maxMessages, std::int64_t timeoutMs)
    {
        if (!replay())
            return;

        bus_.poll(clock_subscription_, maxMessages, timeoutMs);

        // A hard-restarted follower must not depend on a single startup request being
        // observable/deliverable. While unsynchronized, periodically ask the authority
        // to re-publish its current durable state. This uses monotonic process time only.
        if (!synchronized() && std::chrono::steady_clock::now() >= next_sync_request_at_)
            requestSynchronizationBestEffort("unsynchronized_retry");
    }
};
