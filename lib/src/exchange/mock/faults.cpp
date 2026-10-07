// Inject reproducible transport/venue faults around the durable MOCK runtime.
// Observed receipts may be delayed or lost even when the underlying command was applied.

#include "faults.h"

#include <limits>
#include <stdexcept>

namespace MockVenue {

MockFaults::MockFaults(
    std::filesystem::path durable_directory,
    MockChaosConfig chaos_config,
    MatchingFillConfig matching_config,
    MockAccountingConfig accounting_config)
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

void MockFaults::setUserStreamConnected(bool connected, Timestamp event_time)
{
    const std::uint64_t before = runtime_.lastSequence();
    const std::string econ = runtime_.account().economicFingerprint();

    user_stream_connected_ = connected;
    if (connected)
        force_fresh_reconciliation_ = true;

    recordEvidence(
        ChaosOperation::Connectivity,
        ChaosFault::DisconnectUserStream,
        connected
            ? ChaosStatus::Delivered
            : ChaosStatus::UserStreamDisconnected,
        event_time,
        before,
        runtime_.lastSequence(),
        econ,
        runtime_.account().economicFingerprint(),
        ErrorClass::None,
        connected ? "user stream reconnected" : "user stream disconnected");
}

void MockFaults::setVenueAvailable(bool available, Timestamp event_time)
{
    const std::uint64_t before = runtime_.lastSequence();
    const std::string econ = runtime_.account().economicFingerprint();

    venue_available_ = available;
    if (available)
        force_fresh_reconciliation_ = true;

    recordEvidence(
        ChaosOperation::Connectivity,
        ChaosFault::VenueUnavailable,
        available
            ? ChaosStatus::Delivered
            : ChaosStatus::VenueUnavailable,
        event_time,
        before,
        runtime_.lastSequence(),
        econ,
        runtime_.account().economicFingerprint(),
        available ? ErrorClass::None : ErrorClass::VenueUnavailable,
        available ? "venue restored" : "venue unavailable");
}

// A clean report authorizes only the exact stream sequence it reconciled.
// New evidence, a disconnect or an ambiguous submit requires another reconciliation.
bool MockFaults::canRouteNewSubmit() const
{
    return venue_available_ &&
           user_stream_connected_ &&
           !ambiguous_submit_requires_reconciliation_ &&
           !force_fresh_reconciliation_ &&
           last_reconciliation_.clean() &&
           last_reconciled_sequence_ == runtime_.lastSequence() &&
           runtime_.recoverySafe();
}

ReconciliationReport MockFaults::reconcile(Timestamp snapshot_time)
{
    const std::uint64_t before = runtime_.lastSequence();
    const std::string econ_before =
        runtime_.account().economicFingerprint();

    if (!consumeRate(
            ChaosOperation::Reconcile,
            snapshot_time)) {
        ReconciliationReport report =
            pendingReportFromUnavailableEvidence();
        last_reconciliation_ = report;
        last_reconciled_sequence_ = 0U;

        recordEvidence(
            ChaosOperation::Reconcile,
            ChaosFault::None,
            ChaosStatus::RateLimited,
            snapshot_time,
            before,
            runtime_.lastSequence(),
            econ_before,
            runtime_.account().economicFingerprint(),
            ErrorClass::RateLimited,
            "reconciliation read was rate limited");
        return report;
    }

    ChaosFault fault =
        consumeExplicitFault(ChaosOperation::Reconcile);

    if (fault == ChaosFault::VenueUnavailable)
        venue_available_ = false;

    if (!venue_available_ ||
        !user_stream_connected_) {
        ReconciliationReport report =
            pendingReportFromUnavailableEvidence();
        last_reconciliation_ = report;
        last_reconciled_sequence_ = 0U;

        recordEvidence(
            ChaosOperation::Reconcile,
            fault,
            venue_available_
                ? ChaosStatus::UserStreamDisconnected
                : ChaosStatus::VenueUnavailable,
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

    const LocalExpectedState local =
        reconciler_.buildLocalExpected(runtime_.stream());

    UserStateSnapshot venue =
        runtime_.stateSnapshot(
            snapshot_time,
            std::numeric_limits<std::size_t>::max());

    if (fault == ChaosFault::StaleSnapshot) {
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
        ChaosOperation::Reconcile,
        fault,
        last_reconciliation_.clean()
            ? ChaosStatus::Delivered
            : ChaosStatus::BlockedByReconciliation,
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

ChaosOperationResult MockFaults::submit(const SubmitOrderBatch& batch)
{
    return command(
        ChaosOperation::Submit,
        batch.request.request_id,
        batch.request.requested_at,
        [&]() { return runtime_.submit(batch); });
}

ChaosOperationResult MockFaults::cancel(const CancelOrderBatch& batch)
{
    return command(
        ChaosOperation::Cancel,
        batch.request.request_id,
        batch.request.requested_at,
        [&]() { return runtime_.cancel(batch); });
}

ChaosOperationResult MockFaults::modify(const ModifyOrderBatch& batch)
{
    return command(
        ChaosOperation::Modify,
        batch.request.request_id,
        batch.request.requested_at,
        [&]() { return runtime_.modify(batch); });
}

ChaosOperationResult MockFaults::processBar(
    const MarketBarObservation& observation)
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
            "MOCK venue is unavailable");
        recordResultEvidence(
            ChaosOperation::MarketSource,
            observation.event_time,
            before,
            econ_before,
            result);
        return result;
    }

    if (!consumeRate(
            ChaosOperation::MarketSource,
            observation.event_time)) {
        auto result = errorResult(
            ChaosStatus::RateLimited,
            ChaosFault::None,
            ErrorClass::RateLimited,
            true,
            "MOCK market source rate limit");
        recordResultEvidence(
            ChaosOperation::MarketSource,
            observation.event_time,
            before,
            econ_before,
            result);
        return result;
    }

    const ChaosFault fault =
        consumeExplicitFault(
            ChaosOperation::MarketSource);

    if (fault == ChaosFault::VenueUnavailable) {
        venue_available_ = false;
        auto result = errorResult(
            ChaosStatus::VenueUnavailable,
            fault,
            ErrorClass::VenueUnavailable,
            true,
            "injected venue unavailable");
        recordResultEvidence(
            ChaosOperation::MarketSource,
            observation.event_time,
            before,
            econ_before,
            result);
        return result;
    }

    if (fault == ChaosFault::OutOfOrderSource) {
        MarketBarObservation injected =
            observation;
        const Timestamp last =
            runtime_.lastBusinessEventTime();
        injected.event_time =
            last == 0U ? 0U : last - 1U;

        const RecoverySourceResult underlying =
            runtime_.processBar(injected);

        ChaosOperationResult result;
        result.fault = fault;
        result.underlying = underlying;
        result.status =
            underlying == RecoverySourceResult::OutOfOrderRejected
            ? ChaosStatus::OutOfOrderRejected
            : ChaosStatus::UnderlyingRejected;
        result.error = canonicalError(
            ErrorClass::InvalidRequest,
            false,
            "CHAOS_OUT_OF_ORDER_SOURCE",
            "out-of-order source event injected");

        recordResultEvidence(
            ChaosOperation::MarketSource,
            injected.event_time,
            before,
            econ_before,
            result);
        return result;
    }

    const RecoverySourceResult underlying =
        runtime_.processBar(observation);

    ChaosOperationResult result;
    result.fault = fault;
    result.underlying = underlying;

    if (underlying == RecoverySourceResult::Applied) {
        result.status = ChaosStatus::Delivered;
        result.error = noError();

        if (fault == ChaosFault::DuplicateSource) {
            const RecoverySourceResult duplicate =
                runtime_.processBar(observation);
            if (duplicate !=
                RecoverySourceResult::DuplicateIgnored)
                throw std::runtime_error(
                    "Step55 duplicate source injection was not idempotent");
        }
    } else if (
        underlying ==
        RecoverySourceResult::DuplicateIgnored) {
        result.status = ChaosStatus::DuplicateIgnored;
        result.error = noError();
    } else if (
        underlying ==
        RecoverySourceResult::OutOfOrderRejected) {
        result.status = ChaosStatus::OutOfOrderRejected;
        result.error = canonicalError(
            ErrorClass::InvalidRequest,
            false,
            "SOURCE_OUT_OF_ORDER",
            "underlying recovery rejected out-of-order source");
    } else {
        result.status = ChaosStatus::UnderlyingRejected;
        result.error = canonicalError(
            ErrorClass::InvalidRequest,
            false,
            "SOURCE_REJECTED",
            "underlying recovery rejected market source");
    }

    recordResultEvidence(
        ChaosOperation::MarketSource,
        observation.event_time,
        before,
        econ_before,
        result);
    return result;
}

ObservedUserStreamPage MockFaults::streamAfter(
    std::uint64_t after_sequence,
    std::size_t max_events,
    Timestamp observation_time)
{
    const std::uint64_t before =
        runtime_.lastSequence();
    const std::string econ =
        runtime_.account().economicFingerprint();

    ObservedUserStreamPage observed;

    if (!user_stream_connected_ ||
        !venue_available_) {
        observed.available = false;

        recordEvidence(
            ChaosOperation::StreamRead,
            ChaosFault::None,
            !venue_available_
                ? ChaosStatus::VenueUnavailable
                : ChaosStatus::UserStreamDisconnected,
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
            ChaosOperation::StreamRead,
            observation_time)) {
        observed.available = false;

        recordEvidence(
            ChaosOperation::StreamRead,
            ChaosFault::None,
            ChaosStatus::RateLimited,
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
            ChaosOperation::StreamRead);

    if (observed.fault ==
            ChaosFault::DuplicateUserDelivery &&
        !observed.page.events.empty()) {
        observed.page.events.insert(
            observed.page.events.begin() + 1,
            observed.page.events.front());
    } else if (
        observed.fault ==
            ChaosFault::OutOfOrderUserDelivery &&
        observed.page.events.size() >= 2U) {
        std::swap(
            observed.page.events[0],
            observed.page.events[1]);
    } else if (
        observed.fault ==
            ChaosFault::DisconnectUserStream) {
        user_stream_connected_ = false;
        observed.available = false;
        observed.page.events.clear();
    }

    recordEvidence(
        ChaosOperation::StreamRead,
        observed.fault,
        observed.available
            ? ChaosStatus::Delivered
            : ChaosStatus::UserStreamDisconnected,
        observation_time,
        before,
        runtime_.lastSequence(),
        econ,
        runtime_.account().economicFingerprint(),
        ErrorClass::None,
        "observed user-stream page");

    return observed;
}

std::vector<DelayedReceipt> MockFaults::releaseDelayedResponses()
{
    std::vector<DelayedReceipt> released;
    released.swap(delayed_receipts_);
    return released;
}

MockFaultChaosResumeState MockFaults::snapshotResumeState() const
{
    MockFaultChaosResumeState state;
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
            ChaosRateWindowResume{
                window.window,
                window.used,
                window.initialized});
    }
    return state;
}

void MockFaults::restoreResumeState(const MockFaultChaosResumeState& state)
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

std::string MockFaults::evidenceFingerprint() const
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
    return Sha256::hexDigest(out.str());
}

Error MockFaults::noError()
{
    Error error;
    error.classification = ErrorClass::None;
    error.retryable = false;
    return error;
}

Error MockFaults::canonicalError(
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

ChaosOperationResult MockFaults::errorResult(
    ChaosStatus status,
    ChaosFault fault,
    ErrorClass classification,
    bool retryable,
    const std::string& detail)
{
    ChaosOperationResult result;
    result.status = status;
    result.fault = fault;
    result.error = canonicalError(
        classification,
        retryable,
        detail,
        detail);
    return result;
}

std::uint32_t MockFaults::limitFor(
    ChaosOperation operation) const
{
    switch (operation) {
    case ChaosOperation::Submit:
        return config_.submit_limit;
    case ChaosOperation::Cancel:
        return config_.cancel_limit;
    case ChaosOperation::Modify:
        return config_.modify_limit;
    case ChaosOperation::MarketSource:
        return config_.market_source_limit;
    case ChaosOperation::Reconcile:
        return config_.reconcile_limit;
    case ChaosOperation::StreamRead:
        return config_.stream_read_limit;
    case ChaosOperation::Connectivity:
        return std::numeric_limits<std::uint32_t>::max();
    }
    return 1U;
}

// Rate windows use caller-supplied event time, keeping replay results independent
// of machine speed and avoiding wall-clock effects on order admission.
bool MockFaults::consumeRate(
    ChaosOperation operation,
    Timestamp event_time)
{
    if (operation == ChaosOperation::Connectivity)
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

ChaosFault MockFaults::autoSubmitFault()
{
    if (!config_.auto_submit_faults)
        return ChaosFault::None;

    const std::uint32_t draw =
        deterministicPpmDraw(
            config_.seed,
            submit_fault_ordinal_++,
            "SUBMIT");

    std::uint32_t threshold =
        config_.auto_reject_ppm;
    if (draw < threshold)
        return ChaosFault::CanonicalReject;

    threshold +=
        config_.auto_lost_response_ppm;
    if (draw < threshold)
        return ChaosFault::LoseResponseAmbiguousSubmit;

    threshold +=
        config_.auto_delayed_response_ppm;
    if (draw < threshold)
        return ChaosFault::DelayResponse;

    return ChaosFault::None;
}

ChaosFault MockFaults::consumeExplicitFault(
    ChaosOperation operation)
{
    if (!explicit_faults_.empty()) {
        const ChaosFault fault =
            explicit_faults_.front();
        explicit_faults_.erase(
            explicit_faults_.begin());
        return fault;
    }

    return operation == ChaosOperation::Submit
        ? autoSubmitFault()
        : ChaosFault::None;
}

ReconciliationReport MockFaults::pendingReportFromUnavailableEvidence() const
{
    const LocalExpectedState local =
        reconciler_.buildLocalExpected(runtime_.stream());

    UserStateSnapshot unavailable;
    unavailable.recovery_safe =
        runtime_.recoverySafe();

    return reconciler_.compare(
        local,
        unavailable);
}

void MockFaults::recordResultEvidence(
    ChaosOperation operation,
    Timestamp event_time,
    std::uint64_t sequence_before,
    const std::string& economic_before,
    const ChaosOperationResult& result)
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

void MockFaults::recordEvidence(
    ChaosOperation operation,
    ChaosFault fault,
    ChaosStatus status,
    Timestamp event_time,
    std::uint64_t sequence_before,
    std::uint64_t sequence_after,
    const std::string& economic_before,
    const std::string& economic_after,
    ErrorClass error_class,
    const std::string& detail)
{
    ChaosEvidence e;
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

} // namespace MockVenue
