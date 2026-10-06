#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "canonical_venue_account_v1.h"
#include "mock_accounting_fixed_point_v1.h"
#include "mock_sha256_v1.h"
#include "mock_snapshot_user_stream_recovery_v1.h"
#include "mock_venue_catalog_v1.h"
#include "mock_venue_rules_v1.h"

/**************************************************************************************
 * Purpose : Reconcile local MOCK projections with venue snapshots and maintain the
 *           deterministic accounting ledger used as integrity evidence.
 *
 * Reconciliation reports differences explicitly. It must not hide missing orders, fills,
 * positions, cash differences or ledger-integrity failures.
 **************************************************************************************/

namespace MockVenueV1 {

enum class ReconciliationStateV1 {
    Pending = 0,
    Clean,
    Blocked
};

enum class ReconciliationIssueKindV1 {
    MissingVenueEvidence = 0,
    SequenceMismatch,
    RecoveryUnsafe,
    LocalProjectionInvalid,
    LedgerIntegrityMismatch,
    LedgerCashMismatch,
    CashMismatch,
    PositionMissingOnVenue,
    UnexpectedVenuePosition,
    PositionIdentityMismatch,
    PositionQuantityMismatch,
    PositionEntryPriceMismatch,
    OpenOrderMissingOnVenue,
    UnexpectedVenueOpenOrder,
    OpenOrderIdentityMismatch,
    OpenOrderStatusMismatch,
    OpenOrderQuantityMismatch,
    OpenOrderNativeIdentityMismatch,
    FillMissingOnVenue,
    UnexpectedVenueFill,
    FillMismatch
};

struct ReconciliationIssueV1 {
    ReconciliationIssueKindV1 kind =
        ReconciliationIssueKindV1::LocalProjectionInvalid;
    std::string asset;
    OrderID local_order_id = 0;
    std::string fill_id;
    std::string local_value;
    std::string venue_value;
    std::string message;
};

enum class LedgerEntryKindV1 {
    Fill = 0,
    TradingFee,
    Rebate,
    FundingPayment
};

struct LedgerEntryV1 {
    std::uint64_t stream_sequence = 0U;
    LedgerEntryKindV1 kind = LedgerEntryKindV1::Fill;
    std::string entry_id;
    Timestamp event_time = 0;
    std::string asset;
    OrderID local_order_id = 0;
    StrategyID strategy_id = 0;
    std::string native_fill_id;
    std::int64_t position_delta_units = 0;
    std::int64_t cash_delta_units = 0;
    std::int64_t realized_pnl_units = 0;
    std::uint64_t gross_notional_units = 0U;
    std::string previous_hash;
    std::string entry_hash;
};

struct LedgerPositionV1 {
    VenueContracts::V1::InstrumentIdentity instrument;
    std::int64_t signed_quantity_units = 0;
    std::uint64_t average_entry_price_units = 0U;
};

struct LedgerProjectionV1 {
    bool valid = true;
    std::string error;
    std::int64_t settled_cash_units = 0;
    std::map<std::string, LedgerPositionV1> positions;
    std::vector<Fill> fills;
    std::vector<LedgerEntryV1> entries;
    std::string head_hash = std::string(64U, '0');
    std::map<std::string, std::uint64_t> expected_fee_units_by_fill;
    std::set<std::string> fee_fill_ids_seen;
};

struct ExpectedOpenOrderV1 {
    OrderID local_order_id = 0;
    VenueContracts::V1::InstrumentIdentity instrument;
    OrderLifecycleStatus status =
        OrderLifecycleStatus::UnknownRequiresReconciliation;
    std::uint64_t cumulative_filled_units = 0U;
    std::uint64_t remaining_units = 0U;
    NativeReferences native_references;
};

struct LocalExpectedStateV1 {
    bool complete = true;
    std::string error;
    std::uint64_t stream_sequence = 0U;
    LedgerProjectionV1 ledger;
    std::map<OrderID, ExpectedOpenOrderV1> open_orders;
};

struct ReconciliationReportV1 {
    ReconciliationStateV1 state =
        ReconciliationStateV1::Pending;
    std::uint64_t local_sequence = 0U;
    std::uint64_t venue_sequence = 0U;
    std::string ledger_head_hash;
    std::vector<ReconciliationIssueV1> issues;

    bool clean() const
    {
        return state == ReconciliationStateV1::Clean &&
               issues.empty();
    }
};

enum class NewOrderRouteResultV1 {
    Submitted = 0,
    BlockedByReconciliation,
    UnderlyingRejected
};

class MockReconciliationLedgerParityV1 {
public:
    explicit MockReconciliationLedgerParityV1(
            std::uint32_t trading_fee_ppm = 400U);

    LocalExpectedStateV1 buildLocalExpected(
            const std::vector<UserStreamEnvelopeV1>& stream) const;

    ReconciliationReportV1 compare(
            const LocalExpectedStateV1& local,
            const UserStateSnapshotV1& venue) const;

    ReconciliationReportV1 reconcile(
            const MockSnapshotUserStreamRecoveryV1& runtime,
            Timestamp snapshot_time);

    const ReconciliationReportV1& lastReport() const;

    bool canRouteNewOrders(
            const MockSnapshotUserStreamRecoveryV1& runtime) const;

    NewOrderRouteResultV1 submitIfSafe(
            MockSnapshotUserStreamRecoveryV1& runtime,
            const SubmitOrderBatch& batch);

private:
    struct LedgerWorkingPosition {
        VenueContracts::V1::InstrumentIdentity instrument;
        std::int64_t signed_quantity_units = 0;
        std::uint64_t average_entry_price_units = 0U;
    };

    ReconciliationReportV1 last_report_;
    std::uint64_t last_reconciled_sequence_ = 0U;
    std::uint32_t trading_fee_ppm_ = 400U;

    static LedgerProjectionV1 beginLedger();

    static const char* ledgerKindToken(
            LedgerEntryKindV1 kind);

    static bool isActiveOrderStatus(
            OrderLifecycleStatus status);

    static bool sameInstrument(
            const VenueContracts::V1::InstrumentIdentity& a,
            const VenueContracts::V1::InstrumentIdentity& b);

    static int signOf(std::int64_t value);

    static std::uint64_t absUnits(
            std::int64_t value);

    static bool checkedAddSigned(
            std::int64_t a,
            std::int64_t b,
            std::int64_t* out);

    static bool doubleToSignedUnitsExact(
            double value,
            std::size_t scale,
            std::int64_t* out);

    static std::string ledgerCanonical(
            const LedgerEntryV1& entry);

    static void appendLedgerEntry(
            LedgerProjectionV1& ledger,
            LedgerEntryV1 entry);

    static bool applyPositionFill(
            LedgerProjectionV1& ledger,
            const Fill& fill,
            std::uint64_t quantity_units,
            std::uint64_t price_units,
            std::int64_t* realized_pnl_units);

    static bool applyFillLedger(
            LedgerProjectionV1& ledger,
            std::uint64_t stream_sequence,
            const Fill& fill,
            std::uint32_t trading_fee_ppm);

    static bool applyAccountingLedger(
            LedgerProjectionV1& ledger,
            std::uint64_t stream_sequence,
            const AccountingEvent& event);

    static bool finalizeLedger(
            LedgerProjectionV1& ledger);

    static bool applyOrderUpdate(
            LocalExpectedStateV1& local,
            const OrderUpdate& update);

    static void validateLedgerChain(
            const LedgerProjectionV1& ledger,
            ReconciliationReportV1& report);

    static void compareCash(
            const LocalExpectedStateV1& local,
            const UserStateSnapshotV1& venue,
            ReconciliationReportV1& report);

    static void comparePositions(
            const LocalExpectedStateV1& local,
            const UserStateSnapshotV1& venue,
            ReconciliationReportV1& report);

    static void compareOpenOrders(
            const LocalExpectedStateV1& local,
            const UserStateSnapshotV1& venue,
            ReconciliationReportV1& report);

    static void compareFills(
            const LocalExpectedStateV1& local,
            const UserStateSnapshotV1& venue,
            ReconciliationReportV1& report);

    static bool sameFill(
            const Fill& a,
            const Fill& b);

    static const RuleProfile* rulesForAsset(
            const std::string& asset);

    static std::string doubleText(double value);

    static std::string formatUnsignedScaled(
            std::uint64_t units,
            std::size_t scale);

    static std::string fillSummary(
            const Fill& fill);
};

inline const char* toString(
    ReconciliationStateV1 state)
{
    switch (state) {
    case ReconciliationStateV1::Pending:
        return "PENDING";
    case ReconciliationStateV1::Clean:
        return "CLEAN";
    case ReconciliationStateV1::Blocked:
        return "BLOCKED";
    }
    return "BLOCKED";
}

} // namespace MockVenueV1
