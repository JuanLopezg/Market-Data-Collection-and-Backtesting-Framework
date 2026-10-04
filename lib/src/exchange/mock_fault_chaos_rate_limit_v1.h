#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "mock_chaos_prng_v1.h"
#include "mock_reconciliation_ledger_parity_v1.h"
#include "mock_sha256_v1.h"

namespace MockVenueV1 {

enum class ChaosFaultV1 {
    None = 0,
    DelayResponse,
    LoseResponseAmbiguousSubmit,
    CanonicalReject,
    DisconnectUserStream,
    VenueUnavailable,
    StaleSnapshot,
    DuplicateSource,
    OutOfOrderSource,
    DuplicateUserDelivery,
    OutOfOrderUserDelivery
};

enum class ChaosOperationV1 {
    Submit = 0,
    Cancel,
    Modify,
    MarketSource,
    Reconcile,
    StreamRead,
    Connectivity
};

enum class ChaosStatusV1 {
    Delivered = 0,
    DelayedResponse,
    LostResponseAmbiguous,
    CanonicalRejected,
    RateLimited,
    VenueUnavailable,
    UserStreamDisconnected,
    BlockedByReconciliation,
    DuplicateIgnored,
    OutOfOrderRejected,
    UnderlyingRejected
};

struct ChaosOperationResultV1 {
    ChaosStatusV1 status = ChaosStatusV1::UnderlyingRejected;
    ChaosFaultV1 fault = ChaosFaultV1::None;
    RecoverySourceResultV1 underlying = RecoverySourceResultV1::InvalidRejected;
    Error error;

    bool underlying_may_have_applied = false;
    bool requires_reconciliation_before_retry = false;
};

struct DelayedReceiptV1 {
    std::string command_key;
    ChaosOperationV1 operation = ChaosOperationV1::Submit;
    RecoverySourceResultV1 underlying = RecoverySourceResultV1::InvalidRejected;
};

struct ObservedUserStreamPageV1 {
    bool available = true;
    ChaosFaultV1 fault = ChaosFaultV1::None;
    UserStreamPageV1 page;
};

struct ChaosEvidenceV1 {
    std::uint64_t ordinal = 0U;
    ChaosOperationV1 operation = ChaosOperationV1::Submit;
    ChaosFaultV1 fault = ChaosFaultV1::None;
    ChaosStatusV1 status = ChaosStatusV1::UnderlyingRejected;
    Timestamp event_time = 0;
    std::uint64_t sequence_before = 0U;
    std::uint64_t sequence_after = 0U;
    std::string economic_before;
    std::string economic_after;
    ErrorClass error_class = ErrorClass::None;
    std::string detail;
};

struct ChaosRateWindowResumeV1 {
    std::uint64_t window = 0U;
    std::uint32_t used = 0U;
    bool initialized = false;
};

struct MockFaultChaosResumeStateV1 {
    ReconciliationReportV1 last_reconciliation;
    std::uint64_t last_reconciled_sequence = 0U;
    bool user_stream_connected = true;
    bool venue_available = true;
    bool ambiguous_submit_requires_reconciliation = false;
    bool force_fresh_reconciliation = true;
    std::vector<ChaosFaultV1> explicit_faults;
    std::vector<DelayedReceiptV1> delayed_receipts;
    std::vector<ChaosEvidenceV1> evidence;
    std::map<ChaosOperationV1, ChaosRateWindowResumeV1> rate_state;
    std::uint64_t submit_fault_ordinal = 0U;
};

struct MockChaosConfigV1 {
    std::uint64_t seed = 550055ULL;

    bool auto_submit_faults = false;
    std::uint32_t auto_reject_ppm = 150000U;
    std::uint32_t auto_lost_response_ppm = 150000U;
    std::uint32_t auto_delayed_response_ppm = 200000U;

    std::uint64_t rate_window_event_time_units = 10U;
    std::uint32_t submit_limit = 2U;
    std::uint32_t cancel_limit = 4U;
    std::uint32_t modify_limit = 4U;
    std::uint32_t market_source_limit = 64U;
    std::uint32_t reconcile_limit = 16U;
    std::uint32_t stream_read_limit = 64U;

    bool valid() const
    {
        const std::uint64_t ppm_total =
            static_cast<std::uint64_t>(auto_reject_ppm) +
            static_cast<std::uint64_t>(auto_lost_response_ppm) +
            static_cast<std::uint64_t>(auto_delayed_response_ppm);

        return ppm_total <= 1000000ULL &&
               rate_window_event_time_units > 0U &&
               submit_limit > 0U &&
               cancel_limit > 0U &&
               modify_limit > 0U &&
               market_source_limit > 0U &&
               reconcile_limit > 0U &&
               stream_read_limit > 0U;
    }
};

class MockFaultChaosRateLimitV1 {
public:
    explicit MockFaultChaosRateLimitV1(
        std::filesystem::path durable_directory,
        MockChaosConfigV1 chaos_config = {},
        MatchingFillConfigV1 matching_config = {},
        MockAccountingConfigV1 accounting_config = {})
        : config_(std::move(chaos_config)),
          runtime_(
              std::move(durable_directory),
              matching_config,
              accounting_config),
          reconciler_(accounting_config.fee_ppm)
    {
        if (!config_.valid())
            throw std::invalid_argument("invalid MOCK Step55 chaos config");
    }

    MockSnapshotUserStreamRecoveryV1& runtime() { return runtime_; }
    const MockSnapshotUserStreamRecoveryV1& runtime() const { return runtime_; }

    const ReconciliationReportV1& lastReconciliation() const
    {
        return last_reconciliation_;
    }

    const std::vector<ChaosEvidenceV1>& evidence() const
    {
        return evidence_;
    }

    bool userStreamConnected() const { return user_stream_connected_; }
    bool venueAvailable() const { return venue_available_; }
    bool ambiguousSubmitRequiresReconciliation() const
    {
        return ambiguous_submit_requires_reconciliation_;
    }

    void injectNext(ChaosFaultV1 fault)
    {
        explicit_faults_.push_back(fault);
    }

    void setUserStreamConnected(bool connected, Timestamp event_time)
    {
        const std::uint64_t before = runtime_.lastSequence();
        const std::string econ = runtime_.account().economicFingerprint();

        user_stream_connected_ = connected;
        if (connected)
            force_fresh_reconciliation_ = true;

        recordEvidence(
            ChaosOperationV1::Connectivity,
            ChaosFaultV1::DisconnectUserStream,
            connected
                ? ChaosStatusV1::Delivered
                : ChaosStatusV1::UserStreamDisconnected,
            event_time,
            before,
            runtime_.lastSequence(),
            econ,
            runtime_.account().economicFingerprint(),
            ErrorClass::None,
            connected ? "user stream reconnected" : "user stream disconnected");
    }

    void setVenueAvailable(bool available, Timestamp event_time)
    {
        const std::uint64_t before = runtime_.lastSequence();
        const std::string econ = runtime_.account().economicFingerprint();

        venue_available_ = available;
        if (available)
            force_fresh_reconciliation_ = true;

        recordEvidence(
            ChaosOperationV1::Connectivity,
            ChaosFaultV1::VenueUnavailable,
            available
                ? ChaosStatusV1::Delivered
                : ChaosStatusV1::VenueUnavailable,
            event_time,
            before,
            runtime_.lastSequence(),
            econ,
            runtime_.account().economicFingerprint(),
            available ? ErrorClass::None : ErrorClass::VenueUnavailable,
            available ? "venue restored" : "venue unavailable");
    }

    bool canRouteNewSubmit() const
    {
        return venue_available_ &&
               user_stream_connected_ &&
               !ambiguous_submit_requires_reconciliation_ &&
               !force_fresh_reconciliation_ &&
               last_reconciliation_.clean() &&
               last_reconciled_sequence_ == runtime_.lastSequence() &&
               runtime_.recoverySafe();
    }

    ReconciliationReportV1 reconcile(Timestamp snapshot_time)
    {
        const std::uint64_t before = runtime_.lastSequence();
        const std::string econ_before =
            runtime_.account().economicFingerprint();

        if (!consumeRate(
                ChaosOperationV1::Reconcile,
                snapshot_time)) {
            ReconciliationReportV1 report =
                pendingReportFromUnavailableEvidence();
            last_reconciliation_ = report;
            last_reconciled_sequence_ = 0U;

            recordEvidence(
                ChaosOperationV1::Reconcile,
                ChaosFaultV1::None,
                ChaosStatusV1::RateLimited,
                snapshot_time,
                before,
                runtime_.lastSequence(),
                econ_before,
                runtime_.account().economicFingerprint(),
                ErrorClass::RateLimited,
                "reconciliation read was rate limited");
            return report;
        }

        ChaosFaultV1 fault =
            consumeExplicitFault(ChaosOperationV1::Reconcile);

        if (fault == ChaosFaultV1::VenueUnavailable)
            venue_available_ = false;

        if (!venue_available_ ||
            !user_stream_connected_) {
            ReconciliationReportV1 report =
                pendingReportFromUnavailableEvidence();
            last_reconciliation_ = report;
            last_reconciled_sequence_ = 0U;

            recordEvidence(
                ChaosOperationV1::Reconcile,
                fault,
                venue_available_
                    ? ChaosStatusV1::UserStreamDisconnected
                    : ChaosStatusV1::VenueUnavailable,
                snapshot_time,
                before,
                runtime_.lastSequence(),
                econ_before,
                runtime_.account().economicFingerprint(),
                venue_available_
                    ? ErrorClass::TransportUnavailable
                    : ErrorClass::VenueUnavailable,
                "fresh venue evidence unavailable");
            return report;
        }

        const LocalExpectedStateV1 local =
            reconciler_.buildLocalExpected(runtime_.stream());

        UserStateSnapshotV1 venue =
            runtime_.stateSnapshot(
                snapshot_time,
                std::numeric_limits<std::size_t>::max());

        if (fault == ChaosFaultV1::StaleSnapshot) {
            venue.snapshot_sequence =
                venue.snapshot_sequence == 0U
                ? 1U
                : venue.snapshot_sequence - 1U;
        }

        last_reconciliation_ =
            reconciler_.compare(local, venue);

        if (last_reconciliation_.clean()) {
            last_reconciled_sequence_ =
                runtime_.lastSequence();
            ambiguous_submit_requires_reconciliation_ = false;
            force_fresh_reconciliation_ = false;
        } else {
            last_reconciled_sequence_ = 0U;
        }

        recordEvidence(
            ChaosOperationV1::Reconcile,
            fault,
            last_reconciliation_.clean()
                ? ChaosStatusV1::Delivered
                : ChaosStatusV1::BlockedByReconciliation,
            snapshot_time,
            before,
            runtime_.lastSequence(),
            econ_before,
            runtime_.account().economicFingerprint(),
            ErrorClass::None,
            last_reconciliation_.clean()
                ? "reconciliation clean"
                : "reconciliation not clean");
        return last_reconciliation_;
    }

    ChaosOperationResultV1 submit(const SubmitOrderBatch& batch)
    {
        return command(
            ChaosOperationV1::Submit,
            batch.request.request_id,
            batch.request.requested_at,
            [&]() { return runtime_.submit(batch); });
    }

    ChaosOperationResultV1 cancel(const CancelOrderBatch& batch)
    {
        return command(
            ChaosOperationV1::Cancel,
            batch.request.request_id,
            batch.request.requested_at,
            [&]() { return runtime_.cancel(batch); });
    }

    ChaosOperationResultV1 modify(const ModifyOrderBatch& batch)
    {
        return command(
            ChaosOperationV1::Modify,
            batch.request.request_id,
            batch.request.requested_at,
            [&]() { return runtime_.modify(batch); });
    }

    ChaosOperationResultV1 processBar(
        const MarketBarObservationV1& observation)
    {
        const std::uint64_t before =
            runtime_.lastSequence();
        const std::string econ_before =
            runtime_.account().economicFingerprint();

        if (!venue_available_) {
            auto result = errorResult(
                ChaosStatusV1::VenueUnavailable,
                ChaosFaultV1::None,
                ErrorClass::VenueUnavailable,
                true,
                "MOCK venue is unavailable");
            recordResultEvidence(
                ChaosOperationV1::MarketSource,
                observation.event_time,
                before,
                econ_before,
                result);
            return result;
        }

        if (!consumeRate(
                ChaosOperationV1::MarketSource,
                observation.event_time)) {
            auto result = errorResult(
                ChaosStatusV1::RateLimited,
                ChaosFaultV1::None,
                ErrorClass::RateLimited,
                true,
                "MOCK market source rate limit");
            recordResultEvidence(
                ChaosOperationV1::MarketSource,
                observation.event_time,
                before,
                econ_before,
                result);
            return result;
        }

        const ChaosFaultV1 fault =
            consumeExplicitFault(
                ChaosOperationV1::MarketSource);

        if (fault == ChaosFaultV1::VenueUnavailable) {
            venue_available_ = false;
            auto result = errorResult(
                ChaosStatusV1::VenueUnavailable,
                fault,
                ErrorClass::VenueUnavailable,
                true,
                "injected venue unavailable");
            recordResultEvidence(
                ChaosOperationV1::MarketSource,
                observation.event_time,
                before,
                econ_before,
                result);
            return result;
        }

        if (fault == ChaosFaultV1::OutOfOrderSource) {
            MarketBarObservationV1 injected =
                observation;
            const Timestamp last =
                runtime_.lastBusinessEventTime();
            injected.event_time =
                last == 0U ? 0U : last - 1U;

            const RecoverySourceResultV1 underlying =
                runtime_.processBar(injected);

            ChaosOperationResultV1 result;
            result.fault = fault;
            result.underlying = underlying;
            result.status =
                underlying == RecoverySourceResultV1::OutOfOrderRejected
                ? ChaosStatusV1::OutOfOrderRejected
                : ChaosStatusV1::UnderlyingRejected;
            result.error = canonicalError(
                ErrorClass::InvalidRequest,
                false,
                "CHAOS_OUT_OF_ORDER_SOURCE",
                "out-of-order source event injected");

            recordResultEvidence(
                ChaosOperationV1::MarketSource,
                injected.event_time,
                before,
                econ_before,
                result);
            return result;
        }

        const RecoverySourceResultV1 underlying =
            runtime_.processBar(observation);

        ChaosOperationResultV1 result;
        result.fault = fault;
        result.underlying = underlying;

        if (underlying == RecoverySourceResultV1::Applied) {
            result.status = ChaosStatusV1::Delivered;
            result.error = noError();

            if (fault == ChaosFaultV1::DuplicateSource) {
                const RecoverySourceResultV1 duplicate =
                    runtime_.processBar(observation);
                if (duplicate !=
                    RecoverySourceResultV1::DuplicateIgnored)
                    throw std::runtime_error(
                        "Step55 duplicate source injection was not idempotent");
            }
        } else if (
            underlying ==
            RecoverySourceResultV1::DuplicateIgnored) {
            result.status = ChaosStatusV1::DuplicateIgnored;
            result.error = noError();
        } else if (
            underlying ==
            RecoverySourceResultV1::OutOfOrderRejected) {
            result.status = ChaosStatusV1::OutOfOrderRejected;
            result.error = canonicalError(
                ErrorClass::InvalidRequest,
                false,
                "SOURCE_OUT_OF_ORDER",
                "underlying recovery rejected out-of-order source");
        } else {
            result.status = ChaosStatusV1::UnderlyingRejected;
            result.error = canonicalError(
                ErrorClass::InvalidRequest,
                false,
                "SOURCE_REJECTED",
                "underlying recovery rejected market source");
        }

        recordResultEvidence(
            ChaosOperationV1::MarketSource,
            observation.event_time,
            before,
            econ_before,
            result);
        return result;
    }

    ObservedUserStreamPageV1 streamAfter(
        std::uint64_t after_sequence,
        std::size_t max_events,
        Timestamp observation_time)
    {
        const std::uint64_t before =
            runtime_.lastSequence();
        const std::string econ =
            runtime_.account().economicFingerprint();

        ObservedUserStreamPageV1 observed;

        if (!user_stream_connected_ ||
            !venue_available_) {
            observed.available = false;

            recordEvidence(
                ChaosOperationV1::StreamRead,
                ChaosFaultV1::None,
                !venue_available_
                    ? ChaosStatusV1::VenueUnavailable
                    : ChaosStatusV1::UserStreamDisconnected,
                observation_time,
                before,
                runtime_.lastSequence(),
                econ,
                runtime_.account().economicFingerprint(),
                !venue_available_
                    ? ErrorClass::VenueUnavailable
                    : ErrorClass::TransportUnavailable,
                "user stream delivery unavailable");
            return observed;
        }

        if (!consumeRate(
                ChaosOperationV1::StreamRead,
                observation_time)) {
            observed.available = false;

            recordEvidence(
                ChaosOperationV1::StreamRead,
                ChaosFaultV1::None,
                ChaosStatusV1::RateLimited,
                observation_time,
                before,
                runtime_.lastSequence(),
                econ,
                runtime_.account().economicFingerprint(),
                ErrorClass::RateLimited,
                "user stream read rate limited");
            return observed;
        }

        observed.page =
            runtime_.streamAfter(
                after_sequence,
                max_events);

        observed.fault =
            consumeExplicitFault(
                ChaosOperationV1::StreamRead);

        if (observed.fault ==
                ChaosFaultV1::DuplicateUserDelivery &&
            !observed.page.events.empty()) {
            observed.page.events.insert(
                observed.page.events.begin() + 1,
                observed.page.events.front());
        } else if (
            observed.fault ==
                ChaosFaultV1::OutOfOrderUserDelivery &&
            observed.page.events.size() >= 2U) {
            std::swap(
                observed.page.events[0],
                observed.page.events[1]);
        } else if (
            observed.fault ==
                ChaosFaultV1::DisconnectUserStream) {
            user_stream_connected_ = false;
            observed.available = false;
            observed.page.events.clear();
        }

        recordEvidence(
            ChaosOperationV1::StreamRead,
            observed.fault,
            observed.available
                ? ChaosStatusV1::Delivered
                : ChaosStatusV1::UserStreamDisconnected,
            observation_time,
            before,
            runtime_.lastSequence(),
            econ,
            runtime_.account().economicFingerprint(),
            ErrorClass::None,
            "observed user-stream page");

        return observed;
    }

    std::vector<DelayedReceiptV1> releaseDelayedResponses()
    {
        std::vector<DelayedReceiptV1> released;
        released.swap(delayed_receipts_);
        return released;
    }

    MockFaultChaosResumeStateV1 snapshotResumeState() const
    {
        MockFaultChaosResumeStateV1 state;
        state.last_reconciliation = last_reconciliation_;
        state.last_reconciled_sequence = last_reconciled_sequence_;
        state.user_stream_connected = user_stream_connected_;
        state.venue_available = venue_available_;
        state.ambiguous_submit_requires_reconciliation =
            ambiguous_submit_requires_reconciliation_;
        state.force_fresh_reconciliation = force_fresh_reconciliation_;
        state.explicit_faults = explicit_faults_;
        state.delayed_receipts = delayed_receipts_;
        state.evidence = evidence_;
        state.submit_fault_ordinal = submit_fault_ordinal_;
        for (const auto& [operation, window] : rate_state_) {
            state.rate_state.emplace(
                operation,
                ChaosRateWindowResumeV1{
                    window.window,
                    window.used,
                    window.initialized});
        }
        return state;
    }

    void restoreResumeState(const MockFaultChaosResumeStateV1& state)
    {
        last_reconciliation_ = state.last_reconciliation;
        last_reconciled_sequence_ = state.last_reconciled_sequence;
        user_stream_connected_ = state.user_stream_connected;
        venue_available_ = state.venue_available;
        ambiguous_submit_requires_reconciliation_ =
            state.ambiguous_submit_requires_reconciliation;
        force_fresh_reconciliation_ = state.force_fresh_reconciliation;
        explicit_faults_ = state.explicit_faults;
        delayed_receipts_ = state.delayed_receipts;
        evidence_ = state.evidence;
        submit_fault_ordinal_ = state.submit_fault_ordinal;
        rate_state_.clear();
        for (const auto& [operation, window] : state.rate_state) {
            RateWindowState restored;
            restored.window = window.window;
            restored.used = window.used;
            restored.initialized = window.initialized;
            rate_state_.emplace(operation, restored);
        }
    }

    std::string evidenceFingerprint() const
    {
        std::ostringstream out;
        out << "seed=" << config_.seed;

        for (const auto& e : evidence_) {
            out << '\n'
                << e.ordinal << '|'
                << static_cast<int>(e.operation) << '|'
                << static_cast<int>(e.fault) << '|'
                << static_cast<int>(e.status) << '|'
                << e.event_time << '|'
                << e.sequence_before << '|'
                << e.sequence_after << '|'
                << e.economic_before << '|'
                << e.economic_after << '|'
                << static_cast<int>(e.error_class) << '|'
                << e.detail;
        }
        return Sha256V1::hexDigest(out.str());
    }

private:
    struct RateWindowState {
        std::uint64_t window = 0U;
        std::uint32_t used = 0U;
        bool initialized = false;
    };

    MockChaosConfigV1 config_;
    MockSnapshotUserStreamRecoveryV1 runtime_;
    MockReconciliationLedgerParityV1 reconciler_;

    ReconciliationReportV1 last_reconciliation_;
    std::uint64_t last_reconciled_sequence_ = 0U;

    bool user_stream_connected_ = true;
    bool venue_available_ = true;
    bool ambiguous_submit_requires_reconciliation_ = false;
    bool force_fresh_reconciliation_ = true;

    std::vector<ChaosFaultV1> explicit_faults_;
    std::vector<DelayedReceiptV1> delayed_receipts_;
    std::vector<ChaosEvidenceV1> evidence_;

    std::map<ChaosOperationV1, RateWindowState> rate_state_;
    std::uint64_t submit_fault_ordinal_ = 0U;

    static Error noError()
    {
        Error error;
        error.classification = ErrorClass::None;
        error.retryable = false;
        return error;
    }

    static Error canonicalError(
        ErrorClass classification,
        bool retryable,
        const std::string& native_reason,
        const std::string& detail)
    {
        Error error;
        error.classification = classification;
        error.retryable = retryable;
        error.native_code = "MOCK_CHAOS_V1";
        error.native_reason = native_reason;
        error.detail = detail;
        return error;
    }

    static ChaosOperationResultV1 errorResult(
        ChaosStatusV1 status,
        ChaosFaultV1 fault,
        ErrorClass classification,
        bool retryable,
        const std::string& detail)
    {
        ChaosOperationResultV1 result;
        result.status = status;
        result.fault = fault;
        result.error = canonicalError(
            classification,
            retryable,
            detail,
            detail);
        return result;
    }

    std::uint32_t limitFor(
        ChaosOperationV1 operation) const
    {
        switch (operation) {
        case ChaosOperationV1::Submit:
            return config_.submit_limit;
        case ChaosOperationV1::Cancel:
            return config_.cancel_limit;
        case ChaosOperationV1::Modify:
            return config_.modify_limit;
        case ChaosOperationV1::MarketSource:
            return config_.market_source_limit;
        case ChaosOperationV1::Reconcile:
            return config_.reconcile_limit;
        case ChaosOperationV1::StreamRead:
            return config_.stream_read_limit;
        case ChaosOperationV1::Connectivity:
            return std::numeric_limits<std::uint32_t>::max();
        }
        return 1U;
    }

    bool consumeRate(
        ChaosOperationV1 operation,
        Timestamp event_time)
    {
        if (operation == ChaosOperationV1::Connectivity)
            return true;

        const std::uint64_t window =
            static_cast<std::uint64_t>(event_time) /
            config_.rate_window_event_time_units;

        RateWindowState& state =
            rate_state_[operation];

        if (!state.initialized ||
            state.window != window) {
            state.window = window;
            state.used = 0U;
            state.initialized = true;
        }

        const std::uint32_t limit =
            limitFor(operation);

        if (state.used >= limit)
            return false;

        ++state.used;
        return true;
    }

    ChaosFaultV1 autoSubmitFault()
    {
        if (!config_.auto_submit_faults)
            return ChaosFaultV1::None;

        const std::uint32_t draw =
            deterministicPpmDrawV1(
                config_.seed,
                submit_fault_ordinal_++,
                "SUBMIT");

        std::uint32_t threshold =
            config_.auto_reject_ppm;
        if (draw < threshold)
            return ChaosFaultV1::CanonicalReject;

        threshold +=
            config_.auto_lost_response_ppm;
        if (draw < threshold)
            return ChaosFaultV1::LoseResponseAmbiguousSubmit;

        threshold +=
            config_.auto_delayed_response_ppm;
        if (draw < threshold)
            return ChaosFaultV1::DelayResponse;

        return ChaosFaultV1::None;
    }

    ChaosFaultV1 consumeExplicitFault(
        ChaosOperationV1 operation)
    {
        if (!explicit_faults_.empty()) {
            const ChaosFaultV1 fault =
                explicit_faults_.front();
            explicit_faults_.erase(
                explicit_faults_.begin());
            return fault;
        }

        return operation == ChaosOperationV1::Submit
            ? autoSubmitFault()
            : ChaosFaultV1::None;
    }

    template <typename UnderlyingCall>
    ChaosOperationResultV1 command(
        ChaosOperationV1 operation,
        const std::string& command_key,
        Timestamp event_time,
        UnderlyingCall underlying_call)
    {
        const std::uint64_t before =
            runtime_.lastSequence();
        const std::string econ_before =
            runtime_.account().economicFingerprint();

        if (!venue_available_) {
            auto result = errorResult(
                ChaosStatusV1::VenueUnavailable,
                ChaosFaultV1::None,
                ErrorClass::VenueUnavailable,
                true,
                "venue unavailable");
            recordResultEvidence(
                operation,
                event_time,
                before,
                econ_before,
                result);
            return result;
        }

        if (operation == ChaosOperationV1::Submit) {
            if (!user_stream_connected_) {
                auto result = errorResult(
                    ChaosStatusV1::UserStreamDisconnected,
                    ChaosFaultV1::None,
                    ErrorClass::TransportUnavailable,
                    true,
                    "user stream disconnected");
                recordResultEvidence(
                    operation,
                    event_time,
                    before,
                    econ_before,
                    result);
                return result;
            }

            if (!canRouteNewSubmit()) {
                auto result = errorResult(
                    ChaosStatusV1::BlockedByReconciliation,
                    ChaosFaultV1::None,
                    ErrorClass::VenueUnavailable,
                    false,
                    "new submit blocked by reconciliation safety gate");
                recordResultEvidence(
                    operation,
                    event_time,
                    before,
                    econ_before,
                    result);
                return result;
            }
        }

        if (!consumeRate(operation, event_time)) {
            auto result = errorResult(
                ChaosStatusV1::RateLimited,
                ChaosFaultV1::None,
                ErrorClass::RateLimited,
                true,
                "deterministic fixed-window rate limit");
            recordResultEvidence(
                operation,
                event_time,
                before,
                econ_before,
                result);
            return result;
        }

        const ChaosFaultV1 fault =
            consumeExplicitFault(operation);

        if (fault == ChaosFaultV1::DisconnectUserStream) {
            user_stream_connected_ = false;
            auto result = errorResult(
                ChaosStatusV1::UserStreamDisconnected,
                fault,
                ErrorClass::TransportUnavailable,
                true,
                "injected user-stream disconnect");
            recordResultEvidence(
                operation,
                event_time,
                before,
                econ_before,
                result);
            return result;
        }

        if (fault == ChaosFaultV1::VenueUnavailable) {
            venue_available_ = false;
            auto result = errorResult(
                ChaosStatusV1::VenueUnavailable,
                fault,
                ErrorClass::VenueUnavailable,
                true,
                "injected venue unavailable");
            recordResultEvidence(
                operation,
                event_time,
                before,
                econ_before,
                result);
            return result;
        }

        if (fault == ChaosFaultV1::CanonicalReject) {
            auto result = errorResult(
                ChaosStatusV1::CanonicalRejected,
                fault,
                ErrorClass::VenueLimit,
                false,
                "CHAOS_CANONICAL_REJECT");
            recordResultEvidence(
                operation,
                event_time,
                before,
                econ_before,
                result);
            return result;
        }

        const RecoverySourceResultV1 underlying =
            underlying_call();

        ChaosOperationResultV1 result;
        result.fault = fault;
        result.underlying = underlying;

        if (fault == ChaosFaultV1::DelayResponse &&
            underlying == RecoverySourceResultV1::Applied) {
            result.status =
                ChaosStatusV1::DelayedResponse;
            result.error = noError();

            delayed_receipts_.push_back({
                command_key,
                operation,
                underlying
            });
        } else if (
            fault ==
                ChaosFaultV1::LoseResponseAmbiguousSubmit &&
            operation == ChaosOperationV1::Submit &&
            underlying == RecoverySourceResultV1::Applied) {
            result.status =
                ChaosStatusV1::LostResponseAmbiguous;
            result.error = canonicalError(
                ErrorClass::TransportUnavailable,
                false,
                "AMBIGUOUS_SUBMIT_RESPONSE_LOST",
                "submit may have been applied; reconcile before retry");
            result.underlying_may_have_applied = true;
            result.requires_reconciliation_before_retry = true;
            ambiguous_submit_requires_reconciliation_ = true;
        } else if (
            underlying ==
            RecoverySourceResultV1::Applied) {
            result.status =
                ChaosStatusV1::Delivered;
            result.error = noError();
        } else if (
            underlying ==
            RecoverySourceResultV1::DuplicateIgnored) {
            result.status =
                ChaosStatusV1::DuplicateIgnored;
            result.error = noError();
        } else if (
            underlying ==
            RecoverySourceResultV1::OutOfOrderRejected) {
            result.status =
                ChaosStatusV1::OutOfOrderRejected;
            result.error = canonicalError(
                ErrorClass::InvalidRequest,
                false,
                "OUT_OF_ORDER_REJECTED",
                "underlying recovery rejected out-of-order command");
        } else {
            result.status =
                ChaosStatusV1::UnderlyingRejected;
            result.error = canonicalError(
                ErrorClass::InvalidRequest,
                false,
                "UNDERLYING_REJECTED",
                "underlying MOCK runtime rejected command");
        }

        if (operation == ChaosOperationV1::Submit &&
            underlying == RecoverySourceResultV1::Applied)
            force_fresh_reconciliation_ = true;

        recordResultEvidence(
            operation,
            event_time,
            before,
            econ_before,
            result);
        return result;
    }

    ReconciliationReportV1 pendingReportFromUnavailableEvidence() const
    {
        const LocalExpectedStateV1 local =
            reconciler_.buildLocalExpected(runtime_.stream());

        UserStateSnapshotV1 unavailable;
        unavailable.recovery_safe =
            runtime_.recoverySafe();

        return reconciler_.compare(
            local,
            unavailable);
    }

    void recordResultEvidence(
        ChaosOperationV1 operation,
        Timestamp event_time,
        std::uint64_t sequence_before,
        const std::string& economic_before,
        const ChaosOperationResultV1& result)
    {
        recordEvidence(
            operation,
            result.fault,
            result.status,
            event_time,
            sequence_before,
            runtime_.lastSequence(),
            economic_before,
            runtime_.account().economicFingerprint(),
            result.error.classification,
            result.error.detail);
    }

    void recordEvidence(
        ChaosOperationV1 operation,
        ChaosFaultV1 fault,
        ChaosStatusV1 status,
        Timestamp event_time,
        std::uint64_t sequence_before,
        std::uint64_t sequence_after,
        const std::string& economic_before,
        const std::string& economic_after,
        ErrorClass error_class,
        const std::string& detail)
    {
        ChaosEvidenceV1 e;
        e.ordinal =
            static_cast<std::uint64_t>(
                evidence_.size()) + 1U;
        e.operation = operation;
        e.fault = fault;
        e.status = status;
        e.event_time = event_time;
        e.sequence_before = sequence_before;
        e.sequence_after = sequence_after;
        e.economic_before = economic_before;
        e.economic_after = economic_after;
        e.error_class = error_class;
        e.detail = detail;
        evidence_.push_back(std::move(e));
    }
};

} // namespace MockVenueV1
