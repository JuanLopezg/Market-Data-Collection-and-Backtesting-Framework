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

#include "random.h"
#include "reconciliation.h"
#include "hash.h"

// Wrap the MOCK venue with deterministic faults, rate limits and delivery
// disturbances used to test recovery and reconciliation behavior.
//
// Fault injection is deterministic from configuration/seed so the same test scenario can
// be replayed exactly.

namespace MockVenue {

enum class ChaosFault {
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

enum class ChaosOperation {
    Submit = 0,
    Cancel,
    Modify,
    MarketSource,
    Reconcile,
    StreamRead,
    Connectivity
};

enum class ChaosStatus {
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

struct ChaosOperationResult {
    ChaosStatus status = ChaosStatus::UnderlyingRejected;
    ChaosFault fault = ChaosFault::None;
    RecoverySourceResult underlying = RecoverySourceResult::InvalidRejected;
    Error error;

    bool underlying_may_have_applied = false;
    bool requires_reconciliation_before_retry = false;
};

struct DelayedReceipt {
    std::string command_key;
    ChaosOperation operation = ChaosOperation::Submit;
    RecoverySourceResult underlying = RecoverySourceResult::InvalidRejected;
};

struct ObservedUserStreamPage {
    bool available = true;
    ChaosFault fault = ChaosFault::None;
    UserStreamPage page;
};

struct ChaosEvidence {
    std::uint64_t ordinal = 0U;
    ChaosOperation operation = ChaosOperation::Submit;
    ChaosFault fault = ChaosFault::None;
    ChaosStatus status = ChaosStatus::UnderlyingRejected;
    Timestamp event_time = 0;
    std::uint64_t sequence_before = 0U;
    std::uint64_t sequence_after = 0U;
    std::string economic_before;
    std::string economic_after;
    ErrorClass error_class = ErrorClass::None;
    std::string detail;
};

struct ChaosRateWindowResume {
    std::uint64_t window = 0U;
    std::uint32_t used = 0U;
    bool initialized = false;
};

struct MockFaultChaosResumeState {
    ReconciliationReport last_reconciliation;
    std::uint64_t last_reconciled_sequence = 0U;
    bool user_stream_connected = true;
    bool venue_available = true;
    bool ambiguous_submit_requires_reconciliation = false;
    bool force_fresh_reconciliation = true;
    std::vector<ChaosFault> explicit_faults;
    std::vector<DelayedReceipt> delayed_receipts;
    std::vector<ChaosEvidence> evidence;
    std::map<ChaosOperation, ChaosRateWindowResume> rate_state;
    std::uint64_t submit_fault_ordinal = 0U;
};

struct MockChaosConfig {
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

// Public control surface for deterministic fault injection and recovery tests.
// The template command wrapper stays in this header because it accepts arbitrary
// callables; the rest of the implementation lives in the matching .cpp file.
class MockFaults {
public:
    explicit MockFaults(
        std::filesystem::path durable_directory,
        MockChaosConfig chaos_config = {},
        MatchingFillConfig matching_config = {},
        MockAccountingConfig accounting_config = {});

    MockRecovery& runtime() { return runtime_; }
    const MockRecovery& runtime() const { return runtime_; }

    const ReconciliationReport& lastReconciliation() const
    {
        return last_reconciliation_;
    }

    const std::vector<ChaosEvidence>& evidence() const
    {
        return evidence_;
    }

    bool userStreamConnected() const { return user_stream_connected_; }
    bool venueAvailable() const { return venue_available_; }
    bool ambiguousSubmitRequiresReconciliation() const
    {
        return ambiguous_submit_requires_reconciliation_;
    }

    void injectNext(ChaosFault fault)
    {
        explicit_faults_.push_back(fault);
    }

    void setUserStreamConnected(bool connected, Timestamp event_time);

    void setVenueAvailable(bool available, Timestamp event_time);

    bool canRouteNewSubmit() const;

    ReconciliationReport reconcile(Timestamp snapshot_time);

    ChaosOperationResult submit(const SubmitOrderBatch& batch);

    ChaosOperationResult cancel(const CancelOrderBatch& batch);

    ChaosOperationResult modify(const ModifyOrderBatch& batch);

    ChaosOperationResult processBar(
        const MarketBarObservation& observation);

    ObservedUserStreamPage streamAfter(
        std::uint64_t after_sequence,
        std::size_t max_events,
        Timestamp observation_time);

    std::vector<DelayedReceipt> releaseDelayedResponses();

    MockFaultChaosResumeState snapshotResumeState() const;

    void restoreResumeState(const MockFaultChaosResumeState& state);

    std::string evidenceFingerprint() const;

private:
    struct RateWindowState {
        std::uint64_t window = 0U;
        std::uint32_t used = 0U;
        bool initialized = false;
    };

    MockChaosConfig config_;
    MockRecovery runtime_;
    MockReconciliation reconciler_;

    ReconciliationReport last_reconciliation_;
    std::uint64_t last_reconciled_sequence_ = 0U;

    bool user_stream_connected_ = true;
    bool venue_available_ = true;
    bool ambiguous_submit_requires_reconciliation_ = false;
    bool force_fresh_reconciliation_ = true;

    std::vector<ChaosFault> explicit_faults_;
    std::vector<DelayedReceipt> delayed_receipts_;
    std::vector<ChaosEvidence> evidence_;

    std::map<ChaosOperation, RateWindowState> rate_state_;
    std::uint64_t submit_fault_ordinal_ = 0U;

    static Error noError();

    static Error canonicalError(
        ErrorClass classification,
        bool retryable,
        const std::string& native_reason,
        const std::string& detail);

    static ChaosOperationResult errorResult(
        ChaosStatus status,
        ChaosFault fault,
        ErrorClass classification,
        bool retryable,
        const std::string& detail);

    std::uint32_t limitFor(
        ChaosOperation operation) const;

    bool consumeRate(
        ChaosOperation operation,
        Timestamp event_time);

    ChaosFault autoSubmitFault();

    ChaosFault consumeExplicitFault(
        ChaosOperation operation);

    template <typename UnderlyingCall>
    ChaosOperationResult command(
        ChaosOperation operation,
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
                ChaosStatus::VenueUnavailable,
                ChaosFault::None,
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

        if (operation == ChaosOperation::Submit) {
            if (!user_stream_connected_) {
                auto result = errorResult(
                    ChaosStatus::UserStreamDisconnected,
                    ChaosFault::None,
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
                    ChaosStatus::BlockedByReconciliation,
                    ChaosFault::None,
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
                ChaosStatus::RateLimited,
                ChaosFault::None,
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

        const ChaosFault fault =
            consumeExplicitFault(operation);

        if (fault == ChaosFault::DisconnectUserStream) {
            user_stream_connected_ = false;
            auto result = errorResult(
                ChaosStatus::UserStreamDisconnected,
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

        if (fault == ChaosFault::VenueUnavailable) {
            venue_available_ = false;
            auto result = errorResult(
                ChaosStatus::VenueUnavailable,
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

        if (fault == ChaosFault::CanonicalReject) {
            auto result = errorResult(
                ChaosStatus::CanonicalRejected,
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

        // Apply the venue operation before simulating a delayed/lost response.
        // A missing receipt therefore cannot be interpreted as proof that no order exists.
        const RecoverySourceResult underlying =
            underlying_call();

        ChaosOperationResult result;
        result.fault = fault;
        result.underlying = underlying;

        if (fault == ChaosFault::DelayResponse &&
            underlying == RecoverySourceResult::Applied) {
            result.status =
                ChaosStatus::DelayedResponse;
            result.error = noError();

            delayed_receipts_.push_back({
                command_key,
                operation,
                underlying
            });
        } else if (
            fault ==
                ChaosFault::LoseResponseAmbiguousSubmit &&
            operation == ChaosOperation::Submit &&
            underlying == RecoverySourceResult::Applied) {
            result.status =
                ChaosStatus::LostResponseAmbiguous;
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
            RecoverySourceResult::Applied) {
            result.status =
                ChaosStatus::Delivered;
            result.error = noError();
        } else if (
            underlying ==
            RecoverySourceResult::DuplicateIgnored) {
            result.status =
                ChaosStatus::DuplicateIgnored;
            result.error = noError();
        } else if (
            underlying ==
            RecoverySourceResult::OutOfOrderRejected) {
            result.status =
                ChaosStatus::OutOfOrderRejected;
            result.error = canonicalError(
                ErrorClass::InvalidRequest,
                false,
                "OUT_OF_ORDER_REJECTED",
                "underlying recovery rejected out-of-order command");
        } else {
            result.status =
                ChaosStatus::UnderlyingRejected;
            result.error = canonicalError(
                ErrorClass::InvalidRequest,
                false,
                "UNDERLYING_REJECTED",
                "underlying MOCK runtime rejected command");
        }

        if (operation == ChaosOperation::Submit &&
            underlying == RecoverySourceResult::Applied)
            force_fresh_reconciliation_ = true;

        recordResultEvidence(
            operation,
            event_time,
            before,
            econ_before,
            result);
        return result;
    }

    ReconciliationReport pendingReportFromUnavailableEvidence() const;

    void recordResultEvidence(
        ChaosOperation operation,
        Timestamp event_time,
        std::uint64_t sequence_before,
        const std::string& economic_before,
        const ChaosOperationResult& result);

    void recordEvidence(
        ChaosOperation operation,
        ChaosFault fault,
        ChaosStatus status,
        Timestamp event_time,
        std::uint64_t sequence_before,
        std::uint64_t sequence_after,
        const std::string& economic_before,
        const std::string& economic_after,
        ErrorClass error_class,
        const std::string& detail);
};

} // namespace MockVenue
