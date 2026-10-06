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

/**************************************************************************************
 * Purpose : Wrap the MOCK venue with deterministic faults, rate limits and delivery
 *           disturbances used to test recovery and reconciliation behavior.
 *
 * Fault injection is deterministic from configuration/seed so the same test scenario can
 * be replayed exactly.
 **************************************************************************************/

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

// Public control surface for deterministic fault injection and recovery tests.
// The template command wrapper stays in this header because it accepts arbitrary
// callables; the rest of the implementation lives in the matching .cpp file.
class MockFaultChaosRateLimitV1 {
public:
    explicit MockFaultChaosRateLimitV1(
        std::filesystem::path durable_directory,
        MockChaosConfigV1 chaos_config = {},
        MatchingFillConfigV1 matching_config = {},
        MockAccountingConfigV1 accounting_config = {});

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

    void setUserStreamConnected(bool connected, Timestamp event_time);

    void setVenueAvailable(bool available, Timestamp event_time);

    bool canRouteNewSubmit() const;

    ReconciliationReportV1 reconcile(Timestamp snapshot_time);

    ChaosOperationResultV1 submit(const SubmitOrderBatch& batch);

    ChaosOperationResultV1 cancel(const CancelOrderBatch& batch);

    ChaosOperationResultV1 modify(const ModifyOrderBatch& batch);

    ChaosOperationResultV1 processBar(
        const MarketBarObservationV1& observation);

    ObservedUserStreamPageV1 streamAfter(
        std::uint64_t after_sequence,
        std::size_t max_events,
        Timestamp observation_time);

    std::vector<DelayedReceiptV1> releaseDelayedResponses();

    MockFaultChaosResumeStateV1 snapshotResumeState() const;

    void restoreResumeState(const MockFaultChaosResumeStateV1& state);

    std::string evidenceFingerprint() const;

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

    static Error noError();

    static Error canonicalError(
        ErrorClass classification,
        bool retryable,
        const std::string& native_reason,
        const std::string& detail);

    static ChaosOperationResultV1 errorResult(
        ChaosStatusV1 status,
        ChaosFaultV1 fault,
        ErrorClass classification,
        bool retryable,
        const std::string& detail);

    std::uint32_t limitFor(
        ChaosOperationV1 operation) const;

    bool consumeRate(
        ChaosOperationV1 operation,
        Timestamp event_time);

    ChaosFaultV1 autoSubmitFault();

    ChaosFaultV1 consumeExplicitFault(
        ChaosOperationV1 operation);

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

    ReconciliationReportV1 pendingReportFromUnavailableEvidence() const;

    void recordResultEvidence(
        ChaosOperationV1 operation,
        Timestamp event_time,
        std::uint64_t sequence_before,
        const std::string& economic_before,
        const ChaosOperationResultV1& result);

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
        const std::string& detail);
};

} // namespace MockVenueV1
