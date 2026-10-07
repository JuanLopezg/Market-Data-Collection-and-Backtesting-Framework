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

#include "venue_account.h"
#include "decimal.h"
#include "hash.h"
#include "recovery.h"
#include "catalog.h"
#include "rules.h"

// Reconcile local MOCK projections with venue snapshots and maintain the
// deterministic accounting ledger used as integrity evidence.
//
// Reconciliation reports differences explicitly. It must not hide missing orders, fills,
// positions, cash differences or ledger-integrity failures.

namespace MockVenue {

enum class ReconciliationState {
    Pending = 0,
    Clean,
    Blocked
};

enum class ReconciliationIssueKind {
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

struct ReconciliationIssue {
    ReconciliationIssueKind kind =
        ReconciliationIssueKind::LocalProjectionInvalid;
    std::string asset;
    OrderID local_order_id = 0;
    std::string fill_id;
    std::string local_value;
    std::string venue_value;
    std::string message;
};

enum class LedgerEntryKind {
    Fill = 0,
    TradingFee,
    Rebate,
    FundingPayment
};

// One ordered audit entry. Monetary and position deltas use exact scaled units;
// previous_hash links this entry to the preceding one for integrity verification.
struct LedgerEntry {
    std::uint64_t stream_sequence = 0U;
    LedgerEntryKind kind = LedgerEntryKind::Fill;
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

struct LedgerPosition {
    VenueContracts::V1::InstrumentIdentity instrument;
    std::int64_t signed_quantity_units = 0;
    std::uint64_t average_entry_price_units = 0U;
};

struct LedgerProjection {
    bool valid = true;
    std::string error;
    std::int64_t settled_cash_units = 0;
    std::map<std::string, LedgerPosition> positions;
    std::vector<Fill> fills;
    std::vector<LedgerEntry> entries;
    std::string head_hash = std::string(64U, '0'); // Fixed genesis hash for an empty ledger.
    std::map<std::string, std::uint64_t> expected_fee_units_by_fill;
    std::set<std::string> fee_fill_ids_seen;
};

struct ExpectedOpenOrder {
    OrderID local_order_id = 0;
    VenueContracts::V1::InstrumentIdentity instrument;
    OrderLifecycleStatus status =
        OrderLifecycleStatus::UnknownRequiresReconciliation;
    std::uint64_t cumulative_filled_units = 0U;
    std::uint64_t remaining_units = 0U;
    NativeReferences native_references;
};

struct LocalExpectedState {
    bool complete = true;
    std::string error;
    std::uint64_t stream_sequence = 0U;
    LedgerProjection ledger;
    std::map<OrderID, ExpectedOpenOrder> open_orders;
};

struct ReconciliationReport {
    ReconciliationState state =
        ReconciliationState::Pending;
    std::uint64_t local_sequence = 0U;
    std::uint64_t venue_sequence = 0U;
    std::string ledger_head_hash;
    std::vector<ReconciliationIssue> issues;

    bool clean() const
    {
        return state == ReconciliationState::Clean &&
               issues.empty();
    }
};

enum class NewOrderRouteResult {
    Submitted = 0,
    BlockedByReconciliation,
    UnderlyingRejected
};

class MockReconciliation {
public:
    explicit MockReconciliation(
            std::uint32_t trading_fee_ppm = 400U);

    LocalExpectedState buildLocalExpected(
            const std::vector<UserStreamEnvelope>& stream) const;

    ReconciliationReport compare(
            const LocalExpectedState& local,
            const UserStateSnapshot& venue) const;

    ReconciliationReport reconcile(
            const MockRecovery& runtime,
            Timestamp snapshot_time);

    const ReconciliationReport& lastReport() const;

    bool canRouteNewOrders(
            const MockRecovery& runtime) const;

    NewOrderRouteResult submitIfSafe(
            MockRecovery& runtime,
            const SubmitOrderBatch& batch);

private:
    struct LedgerWorkingPosition {
        VenueContracts::V1::InstrumentIdentity instrument;
        std::int64_t signed_quantity_units = 0;
        std::uint64_t average_entry_price_units = 0U;
    };

    ReconciliationReport last_report_;
    std::uint64_t last_reconciled_sequence_ = 0U;
    std::uint32_t trading_fee_ppm_ = 400U;

    static LedgerProjection beginLedger();

    static const char* ledgerKindToken(
            LedgerEntryKind kind);

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
            const LedgerEntry& entry);

    static void appendLedgerEntry(
            LedgerProjection& ledger,
            LedgerEntry entry);

    static bool applyPositionFill(
            LedgerProjection& ledger,
            const Fill& fill,
            std::uint64_t quantity_units,
            std::uint64_t price_units,
            std::int64_t* realized_pnl_units);

    static bool applyFillLedger(
            LedgerProjection& ledger,
            std::uint64_t stream_sequence,
            const Fill& fill,
            std::uint32_t trading_fee_ppm);

    static bool applyAccountingLedger(
            LedgerProjection& ledger,
            std::uint64_t stream_sequence,
            const AccountingEvent& event);

    static bool finalizeLedger(
            LedgerProjection& ledger);

    static bool applyOrderUpdate(
            LocalExpectedState& local,
            const OrderUpdate& update);

    static void validateLedgerChain(
            const LedgerProjection& ledger,
            ReconciliationReport& report);

    static void compareCash(
            const LocalExpectedState& local,
            const UserStateSnapshot& venue,
            ReconciliationReport& report);

    static void comparePositions(
            const LocalExpectedState& local,
            const UserStateSnapshot& venue,
            ReconciliationReport& report);

    static void compareOpenOrders(
            const LocalExpectedState& local,
            const UserStateSnapshot& venue,
            ReconciliationReport& report);

    static void compareFills(
            const LocalExpectedState& local,
            const UserStateSnapshot& venue,
            ReconciliationReport& report);

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
    ReconciliationState state)
{
    switch (state) {
    case ReconciliationState::Pending:
        return "PENDING";
    case ReconciliationState::Clean:
        return "CLEAN";
    case ReconciliationState::Blocked:
        return "BLOCKED";
    }
    return "BLOCKED";
}

} // namespace MockVenue
