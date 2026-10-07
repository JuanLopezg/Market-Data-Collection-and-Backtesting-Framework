// Independently reconstruct local order/account projections from ordered venue events,
// then compare them with venue snapshots and verify the accounting ledger hash chain.

#include "reconciliation.h"

// Implementation for deterministic MOCK reconciliation and ledger projection.
//
// The public types and interface stay in the header. The detailed accounting,
// comparison and hashing logic lives here so callers can understand the contract
// without reading more than a thousand lines of implementation.

namespace MockVenue {

MockReconciliation::MockReconciliation(
        std::uint32_t trading_fee_ppm)
        : trading_fee_ppm_(trading_fee_ppm)
{
    if (trading_fee_ppm_ > 1000000U)
        throw std::invalid_argument(
            "invalid MOCK reconciliation trading fee ppm");
}

LocalExpectedState MockReconciliation::buildLocalExpected(
        const std::vector<UserStreamEnvelope>& stream) const
{
    LocalExpectedState local;
    local.ledger = beginLedger();

    std::uint64_t expected_sequence = 1U;

    for (const auto& envelope : stream) {
        if (envelope.sequence != expected_sequence) {
            local.complete = false;
            local.error =
                "canonical user stream sequence is not contiguous";
            local.stream_sequence =
                envelope.sequence;
            local.ledger.valid = false;
            local.ledger.error = local.error;
            return local;
        }

        local.stream_sequence = envelope.sequence;
        ++expected_sequence;

        if (std::holds_alternative<OrderUpdate>(envelope.event)) {
            if (!applyOrderUpdate(
                    local,
                    std::get<OrderUpdate>(envelope.event))) {
                local.complete = false;
                if (local.error.empty())
                    local.error =
                        "invalid order update in canonical user stream";
                local.ledger.valid = false;
                if (local.ledger.error.empty())
                    local.ledger.error = local.error;
                return local;
            }
            continue;
        }

        if (std::holds_alternative<Fill>(envelope.event)) {
            if (!applyFillLedger(
                    local.ledger,
                    envelope.sequence,
                    std::get<Fill>(envelope.event),
                    trading_fee_ppm_)) {
                local.complete = false;
                local.error = local.ledger.error;
                return local;
            }
            continue;
        }

        if (std::holds_alternative<AccountingEvent>(envelope.event)) {
            if (!applyAccountingLedger(
                    local.ledger,
                    envelope.sequence,
                    std::get<AccountingEvent>(envelope.event))) {
                local.complete = false;
                local.error = local.ledger.error;
                return local;
            }
            continue;
        }

        // OperationResult is intentionally non-economic. Snapshot read surfaces are
        // also ignored if a future stream implementation ever emits them.
    }

    if (!finalizeLedger(local.ledger)) {
        local.complete = false;
        local.error = local.ledger.error;
    }

    return local;
}

ReconciliationReport MockReconciliation::compare(
        const LocalExpectedState& local,
        const UserStateSnapshot& venue) const
{
    ReconciliationReport report;
    report.local_sequence = local.stream_sequence;
    report.venue_sequence = venue.snapshot_sequence;
    report.ledger_head_hash = local.ledger.head_hash;

    if (!local.complete || !local.ledger.valid) {
        report.state = ReconciliationState::Blocked;
        report.issues.push_back({
            ReconciliationIssueKind::LocalProjectionInvalid,
            {},
            0,
            {},
            local.error.empty() ? local.ledger.error : local.error,
            {},
            "local expected projection is invalid"
        });
        return report;
    }

    if (!venue.recovery_safe) {
        report.state = ReconciliationState::Blocked;
        report.issues.push_back({
            ReconciliationIssueKind::RecoveryUnsafe,
            {},
            0,
            {},
            "safe",
            "unsafe",
            "venue recovery state is unsafe"
        });
        return report;
    }

    if (!venue.account.valid() ||
        !venue.open_orders.valid() ||
        !venue.fills.valid()) {
        report.state = ReconciliationState::Pending;
        report.issues.push_back({
            ReconciliationIssueKind::MissingVenueEvidence,
            {},
            0,
            {},
            "complete snapshot",
            "missing/invalid snapshot",
            "venue reconciliation evidence is incomplete"
        });
        return report;
    }

    if (local.stream_sequence != venue.snapshot_sequence) {
        report.state = ReconciliationState::Pending;
        report.issues.push_back({
            ReconciliationIssueKind::SequenceMismatch,
            {},
            0,
            {},
            std::to_string(local.stream_sequence),
            std::to_string(venue.snapshot_sequence),
            "local expected state and venue snapshot are not from the same sequence"
        });
        return report;
    }

    validateLedgerChain(local.ledger, report);
    compareCash(local, venue, report);
    comparePositions(local, venue, report);
    compareOpenOrders(local, venue, report);
    compareFills(local, venue, report);

    report.state =
        report.issues.empty()
        ? ReconciliationState::Clean
        : ReconciliationState::Blocked;
    return report;
}

ReconciliationReport MockReconciliation::reconcile(
        const MockRecovery& runtime,
        Timestamp snapshot_time)
{
    const LocalExpectedState local =
        buildLocalExpected(runtime.stream());

    const UserStateSnapshot venue =
        runtime.stateSnapshot(
            snapshot_time,
            std::numeric_limits<std::size_t>::max());

    last_report_ = compare(local, venue);
    last_reconciled_sequence_ =
        last_report_.clean()
        ? venue.snapshot_sequence
        : 0U;
    return last_report_;
}

const ReconciliationReport& MockReconciliation::lastReport() const
{
    return last_report_;
}

bool MockReconciliation::canRouteNewOrders(
        const MockRecovery& runtime) const
{
    return last_report_.clean() &&
           runtime.recoverySafe() &&
           last_reconciled_sequence_ ==
               runtime.lastSequence();
}

NewOrderRouteResult MockReconciliation::submitIfSafe(
        MockRecovery& runtime,
        const SubmitOrderBatch& batch)
{
    if (!canRouteNewOrders(runtime))
        return NewOrderRouteResult::BlockedByReconciliation;

    const RecoverySourceResult result =
        runtime.submit(batch);

    return result == RecoverySourceResult::Applied
        ? NewOrderRouteResult::Submitted
        : NewOrderRouteResult::UnderlyingRejected;
}

LedgerProjection MockReconciliation::beginLedger()
{
    LedgerProjection ledger;
    if (!parseSignedDecimalToScale(
            "100000.00",
            kMockMoneyScale,
            &ledger.settled_cash_units)) {
        ledger.valid = false;
        ledger.error =
            "unable to parse Step54 initial cash";
    }
    return ledger;
}

const char* MockReconciliation::ledgerKindToken(
        LedgerEntryKind kind)
{
    switch (kind) {
    case LedgerEntryKind::Fill:
        return "FILL";
    case LedgerEntryKind::TradingFee:
        return "TRADING_FEE";
    case LedgerEntryKind::Rebate:
        return "REBATE";
    case LedgerEntryKind::FundingPayment:
        return "FUNDING_PAYMENT";
    }
    return "UNKNOWN";
}

bool MockReconciliation::isActiveOrderStatus(
        OrderLifecycleStatus status)
{
    return status == OrderLifecycleStatus::Accepted ||
           status == OrderLifecycleStatus::Resting ||
           status == OrderLifecycleStatus::PartiallyFilled ||
           status == OrderLifecycleStatus::CancelPending;
}

bool MockReconciliation::sameInstrument(
        const VenueContracts::V1::InstrumentIdentity& a,
        const VenueContracts::V1::InstrumentIdentity& b)
{
    return a.market.canonical_asset ==
               b.market.canonical_asset &&
           a.market.product_class ==
               b.market.product_class &&
           a.market.quote_or_settlement_asset ==
               b.market.quote_or_settlement_asset &&
           a.market.contract_variant ==
               b.market.contract_variant &&
           a.venue_symbol ==
               b.venue_symbol &&
           a.venue_asset_id ==
               b.venue_asset_id;
}

int MockReconciliation::signOf(std::int64_t value)
{
    return value > 0 ? 1 : (value < 0 ? -1 : 0);
}

std::uint64_t MockReconciliation::absUnits(
        std::int64_t value)
{
    if (value ==
        std::numeric_limits<std::int64_t>::min())
        throw std::overflow_error(
            "Step54 signed magnitude overflow");
    return value < 0
        ? static_cast<std::uint64_t>(-value)
        : static_cast<std::uint64_t>(value);
}

bool MockReconciliation::checkedAddSigned(
        std::int64_t a,
        std::int64_t b,
        std::int64_t* out)
{
    if (out == nullptr)
        return false;

    if ((b > 0 &&
         a > std::numeric_limits<std::int64_t>::max() - b) ||
        (b < 0 &&
         a < std::numeric_limits<std::int64_t>::min() - b))
        return false;

    *out = a + b;
    return true;
}

bool MockReconciliation::doubleToSignedUnitsExact(
        double value,
        std::size_t scale,
        std::int64_t* out)
{
    if (out == nullptr ||
        !std::isfinite(value))
        return false;

    char buffer[128];
    const auto result = std::to_chars(
        buffer,
        buffer + sizeof(buffer),
        value,
        std::chars_format::general);
    if (result.ec != std::errc())
        return false;

    return parseSignedDecimalToScale(
        std::string(buffer, result.ptr),
        scale,
        out);
}

std::string MockReconciliation::ledgerCanonical(
        const LedgerEntry& entry)
{
    std::ostringstream out;
    out << entry.previous_hash << '|'
        << entry.stream_sequence << '|'
        << ledgerKindToken(entry.kind) << '|'
        << entry.entry_id << '|'
        << entry.event_time << '|'
        << entry.asset << '|'
        << entry.local_order_id << '|'
        << entry.strategy_id << '|'
        << entry.native_fill_id << '|'
        << entry.position_delta_units << '|'
        << entry.cash_delta_units << '|'
        << entry.realized_pnl_units << '|'
        << entry.gross_notional_units;
    return out.str();
}

void MockReconciliation::appendLedgerEntry(
        LedgerProjection& ledger,
        LedgerEntry entry)
{
    entry.previous_hash =
        ledger.entries.empty()
        ? std::string(64U, '0')
        : ledger.entries.back().entry_hash;
    entry.entry_hash =
        Sha256::hexDigest(
            ledgerCanonical(entry));
    ledger.head_hash =
        entry.entry_hash;
    ledger.entries.push_back(
        std::move(entry));
}

bool MockReconciliation::applyPositionFill(
        LedgerProjection& ledger,
        const Fill& fill,
        std::uint64_t quantity_units,
        std::uint64_t price_units,
        std::int64_t* realized_pnl_units)
{
    if (realized_pnl_units == nullptr)
        return false;
    *realized_pnl_units = 0;

    const std::string asset =
        fill.instrument.market.canonical_asset;

    LedgerPosition& state =
        ledger.positions[asset];
    if (!state.instrument.valid())
        state.instrument =
            fill.instrument;
    else if (!sameInstrument(
                state.instrument,
                fill.instrument))
        return false;

    if (quantity_units >
        static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max()))
        return false;

    const std::int64_t signed_delta =
        fill.side == Side::Buy
        ? static_cast<std::int64_t>(
            quantity_units)
        : -static_cast<std::int64_t>(
            quantity_units);

    const std::int64_t old_q =
        state.signed_quantity_units;
    const int old_sign =
        signOf(old_q);
    const int delta_sign =
        signOf(signed_delta);
    const std::uint64_t delta_abs =
        absUnits(signed_delta);

    if (old_q == 0 ||
        old_sign == delta_sign) {
        const std::uint64_t old_abs =
            absUnits(old_q);

        std::uint64_t new_average = 0U;
        if (!weightedAveragePriceUnits(
                state.average_entry_price_units,
                old_abs,
                price_units,
                delta_abs,
                &new_average))
            return false;

        std::int64_t new_q = 0;
        if (!checkedAddSigned(
                old_q,
                signed_delta,
                &new_q))
            return false;

        state.signed_quantity_units =
            new_q;
        state.average_entry_price_units =
            new_average;
        return true;
    }

    const std::uint64_t old_abs =
        absUnits(old_q);
    const std::uint64_t closing =
        std::min(old_abs, delta_abs);

    const std::int64_t price_difference =
        price_units >=
            state.average_entry_price_units
        ? static_cast<std::int64_t>(
            price_units -
            state.average_entry_price_units)
        : -static_cast<std::int64_t>(
            state.average_entry_price_units -
            price_units);

    std::int64_t close_pnl = 0;
    if (!signedPriceQuantityPnlUnits(
            price_difference,
            closing,
            &close_pnl))
        return false;

    if (old_sign < 0)
        close_pnl = -close_pnl;

    std::int64_t new_q = 0;
    if (!checkedAddSigned(
            old_q,
            signed_delta,
            &new_q))
        return false;

    state.signed_quantity_units =
        new_q;
    *realized_pnl_units =
        close_pnl;

    if (new_q == 0) {
        state.average_entry_price_units =
            0U;
    } else if (
        signOf(new_q) != old_sign) {
        state.average_entry_price_units =
            price_units;
    }

    return true;
}

bool MockReconciliation::applyFillLedger(
        LedgerProjection& ledger,
        std::uint64_t stream_sequence,
        const Fill& fill,
        std::uint32_t trading_fee_ppm)
{
    if (!fill.valid() ||
        fill.venue.venue_id != "MOCK" ||
        fill.venue.environment !=
            VenueContracts::VenueEnvironment::Mock) {
        ledger.valid = false;
        ledger.error =
            "invalid/non-MOCK fill in local ledger stream";
        return false;
    }

    const CatalogEntry* entry =
        findByCanonicalAssetExact(
            fill.instrument.market.canonical_asset);
    const RuleProfile* rules =
        entry == nullptr
        ? nullptr
        : rulesForExact(*entry);

    if (entry == nullptr ||
        rules == nullptr ||
        !entry->enabled ||
        !sameInstrument(
            fill.instrument,
            entry->instrument())) {
        ledger.valid = false;
        ledger.error =
            "fill identity does not match explicit MOCK catalog";
        return false;
    }

    for (const auto& existing : ledger.fills) {
        if (existing.native_references.native_fill_id ==
            fill.native_references.native_fill_id) {
            ledger.valid = false;
            ledger.error =
                "duplicate native_fill_id in canonical user stream";
            return false;
        }
    }

    std::uint64_t quantity_units = 0U;
    std::uint64_t price_units = 0U;

    if (!divisibleByIncrement(
            fill.quantity,
            rules->size_increment,
            rules->size_scale,
            &quantity_units) ||
        !divisibleByIncrement(
            fill.price,
            rules->price_increment,
            rules->price_scale,
            &price_units)) {
        ledger.valid = false;
        ledger.error =
            "fill left frozen Step49 price/size grid";
        return false;
    }

    std::uint64_t notional_units = 0U;
    std::uint64_t expected_fee_units = 0U;

    if (!moneyNotionalUnits(
            price_units,
            quantity_units,
            &notional_units) ||
        !ppmAmountUnits(
            notional_units,
            trading_fee_ppm,
            &expected_fee_units)) {
        ledger.valid = false;
        ledger.error =
            "fill notional/fee overflow";
        return false;
    }

    std::int64_t realized_pnl = 0;
    if (!applyPositionFill(
            ledger,
            fill,
            quantity_units,
            price_units,
            &realized_pnl)) {
        ledger.valid = false;
        ledger.error =
            "unable to project fill into ledger position state";
        return false;
    }

    if (!checkedAddSigned(
            ledger.settled_cash_units,
            realized_pnl,
            &ledger.settled_cash_units)) {
        ledger.valid = false;
        ledger.error =
            "ledger settled cash overflow on realized PnL";
        return false;
    }

    LedgerEntry ledger_entry;
    ledger_entry.stream_sequence =
        stream_sequence;
    ledger_entry.kind =
        LedgerEntryKind::Fill;
    ledger_entry.entry_id =
        "fill:" +
        fill.native_references.native_fill_id;
    ledger_entry.event_time =
        fill.timestamp;
    ledger_entry.asset =
        fill.instrument.market.canonical_asset;
    ledger_entry.local_order_id =
        fill.local_order_id;
    ledger_entry.strategy_id =
        fill.strategy_id;
    ledger_entry.native_fill_id =
        fill.native_references.native_fill_id;
    ledger_entry.position_delta_units =
        fill.side == Side::Buy
        ? static_cast<std::int64_t>(
            quantity_units)
        : -static_cast<std::int64_t>(
            quantity_units);
    ledger_entry.cash_delta_units =
        realized_pnl;
    ledger_entry.realized_pnl_units =
        realized_pnl;
    ledger_entry.gross_notional_units =
        notional_units;

    appendLedgerEntry(
        ledger,
        std::move(ledger_entry));

    if (expected_fee_units > 0U) {
        ledger.expected_fee_units_by_fill.emplace(
            fill.native_references.native_fill_id,
            expected_fee_units);
    }
    ledger.fills.push_back(fill);
    return true;
}

bool MockReconciliation::applyAccountingLedger(
        LedgerProjection& ledger,
        std::uint64_t stream_sequence,
        const AccountingEvent& event)
{
    if (!event.valid() ||
        event.venue.venue_id != "MOCK" ||
        event.venue.environment !=
            VenueContracts::VenueEnvironment::Mock ||
        event.settlement_asset != "USD" ||
        event.correlation_id.empty()) {
        ledger.valid = false;
        ledger.error =
            "invalid/non-MOCK accounting event in local ledger stream";
        return false;
    }

    std::int64_t amount_units = 0;
    if (!parseSignedDecimalToScale(
            event.amount,
            kMockMoneyScale,
            &amount_units)) {
        ledger.valid = false;
        ledger.error =
            "accounting amount is not exact Step52 fixed point";
        return false;
    }

    LedgerEntryKind kind =
        LedgerEntryKind::FundingPayment;

    if (event.type ==
        AccountingEventType::TradingFee) {
        kind =
            LedgerEntryKind::TradingFee;

        if (amount_units >= 0 ||
            event.native_references.native_fill_id.empty() ||
            event.correlation_id !=
                "fee:" +
                event.native_references.native_fill_id) {
            ledger.valid = false;
            ledger.error =
                "trading fee identity/sign is inconsistent with canonical fill";
            return false;
        }

        const auto expected =
            ledger.expected_fee_units_by_fill.find(
                event.native_references.native_fill_id);

        if (expected ==
                ledger.expected_fee_units_by_fill.end() ||
            expected->second >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max()) ||
            -amount_units !=
                static_cast<std::int64_t>(
                    expected->second) ||
            !ledger.fee_fill_ids_seen.insert(
                event.native_references.native_fill_id).second) {
            ledger.valid = false;
            ledger.error =
                "trading fee does not have exact one-to-one fill parity";
            return false;
        }
    } else if (
        event.type ==
        AccountingEventType::Rebate) {
        kind = LedgerEntryKind::Rebate;
        if (amount_units <= 0) {
            ledger.valid = false;
            ledger.error =
                "rebate must increase account value";
            return false;
        }
    } else {
        kind =
            LedgerEntryKind::FundingPayment;
    }

    for (const auto& existing : ledger.entries) {
        if (existing.kind !=
                LedgerEntryKind::Fill &&
            existing.entry_id ==
                "accounting:" +
                event.correlation_id) {
            ledger.valid = false;
            ledger.error =
                "duplicate accounting correlation id in ledger";
            return false;
        }
    }

    if (!checkedAddSigned(
            ledger.settled_cash_units,
            amount_units,
            &ledger.settled_cash_units)) {
        ledger.valid = false;
        ledger.error =
            "ledger settled cash overflow on accounting event";
        return false;
    }

    LedgerEntry ledger_entry;
    ledger_entry.stream_sequence =
        stream_sequence;
    ledger_entry.kind =
        kind;
    ledger_entry.entry_id =
        "accounting:" +
        event.correlation_id;
    ledger_entry.event_time =
        event.timestamp;
    ledger_entry.asset =
        event.market
        ? event.market->canonical_asset
        : std::string{};
    ledger_entry.local_order_id = 0;
    ledger_entry.strategy_id = 0;
    ledger_entry.native_fill_id =
        event.native_references.native_fill_id;
    ledger_entry.position_delta_units = 0;
    ledger_entry.cash_delta_units =
        amount_units;
    ledger_entry.realized_pnl_units = 0;
    ledger_entry.gross_notional_units = 0U;

    appendLedgerEntry(
        ledger,
        std::move(ledger_entry));
    return true;
}

bool MockReconciliation::finalizeLedger(
        LedgerProjection& ledger)
{
    if (!ledger.valid)
        return false;

    if (ledger.expected_fee_units_by_fill.size() !=
        ledger.fee_fill_ids_seen.size()) {
        ledger.valid = false;
        ledger.error =
            "one or more fills lack exactly one canonical trading-fee event";
        return false;
    }

    for (const auto& pair :
         ledger.expected_fee_units_by_fill) {
        if (ledger.fee_fill_ids_seen.find(
                pair.first) ==
            ledger.fee_fill_ids_seen.end()) {
            ledger.valid = false;
            ledger.error =
                "canonical fill lacks trading-fee ledger parity";
            return false;
        }
    }

    return true;
}

bool MockReconciliation::applyOrderUpdate(
        LocalExpectedState& local,
        const OrderUpdate& update)
{
    if (!update.valid() ||
        update.venue.venue_id != "MOCK" ||
        update.venue.environment !=
            VenueContracts::VenueEnvironment::Mock)
        return false;

    if (update.status ==
        OrderLifecycleStatus::UnknownRequiresReconciliation) {
        local.complete = false;
        local.error =
            "order entered UNKNOWN_REQUIRES_RECONCILIATION";
        return false;
    }

    const CatalogEntry* entry =
        findByCanonicalAssetExact(
            update.instrument.market.canonical_asset);
    const RuleProfile* rules =
        entry == nullptr
        ? nullptr
        : rulesForExact(*entry);

    if (entry == nullptr ||
        rules == nullptr ||
        !sameInstrument(
            update.instrument,
            entry->instrument()))
        return false;

    std::uint64_t cumulative_units = 0U;
    std::uint64_t remaining_units = 0U;

    if (!doubleToScaledUnitsExact(
            update.cumulative_filled_quantity,
            rules->size_scale,
            &cumulative_units) ||
        !doubleToScaledUnitsExact(
            update.remaining_quantity,
            rules->size_scale,
            &remaining_units))
        return false;

    if (isActiveOrderStatus(update.status)) {
        ExpectedOpenOrder expected;
        expected.local_order_id =
            update.local_order_id;
        expected.instrument =
            update.instrument;
        expected.status =
            update.status;
        expected.cumulative_filled_units =
            cumulative_units;
        expected.remaining_units =
            remaining_units;
        expected.native_references =
            update.native_references;
        local.open_orders[
            update.local_order_id] =
            std::move(expected);
    } else {
        local.open_orders.erase(
            update.local_order_id);
    }

    return true;
}

// Recompute every entry hash and link; comparing only the final cash balance
// would miss reordered, removed or modified history.
void MockReconciliation::validateLedgerChain(
        const LedgerProjection& ledger,
        ReconciliationReport& report)
{
    std::string previous(64U, '0');

    for (const auto& entry :
         ledger.entries) {
        LedgerEntry copy = entry;
        copy.previous_hash =
            previous;
        copy.entry_hash.clear();

        const std::string expected =
            Sha256::hexDigest(
                ledgerCanonical(copy));

        if (entry.previous_hash !=
                previous ||
            entry.entry_hash !=
                expected) {
            report.issues.push_back({
                ReconciliationIssueKind::LedgerIntegrityMismatch,
                entry.asset,
                entry.local_order_id,
                entry.native_fill_id,
                expected,
                entry.entry_hash,
                "ledger SHA-256 chain is not internally consistent"
            });
            return;
        }

        previous = entry.entry_hash;
    }

    const std::string expected_head =
        ledger.entries.empty()
        ? std::string(64U, '0')
        : previous;

    if (ledger.head_hash !=
        expected_head) {
        report.issues.push_back({
            ReconciliationIssueKind::LedgerIntegrityMismatch,
            {},
            0,
            {},
            expected_head,
            ledger.head_hash,
            "ledger head hash does not match final chain entry"
        });
    }
}

void MockReconciliation::compareCash(
        const LocalExpectedState& local,
        const UserStateSnapshot& venue,
        ReconciliationReport& report)
{
    const Balance* settlement = nullptr;
    for (const auto& balance :
         venue.account.balances) {
        if (balance.asset == "USD") {
            settlement = &balance;
            break;
        }
    }

    if (settlement == nullptr) {
        report.issues.push_back({
            ReconciliationIssueKind::CashMismatch,
            "USD",
            0,
            {},
            formatSignedScaled(
                local.ledger.settled_cash_units,
                kMockMoneyScale),
            "<missing>",
            "venue account snapshot has no settlement cash balance"
        });
        return;
    }

    std::int64_t venue_cash = 0;
    if (!doubleToSignedUnitsExact(
            settlement->total,
            kMockMoneyScale,
            &venue_cash) ||
        venue_cash !=
            local.ledger.settled_cash_units) {
        report.issues.push_back({
            ReconciliationIssueKind::CashMismatch,
            "USD",
            0,
            {},
            formatSignedScaled(
                local.ledger.settled_cash_units,
                kMockMoneyScale),
            doubleText(
                settlement->total),
            "ledger-derived settled cash does not match venue snapshot"
        });
    }
}

void MockReconciliation::comparePositions(
        const LocalExpectedState& local,
        const UserStateSnapshot& venue,
        ReconciliationReport& report)
{
    std::map<std::string, Position>
        venue_positions;

    for (const auto& position :
         venue.account.positions) {
        const std::string asset =
            position.instrument.market.canonical_asset;
        if (asset.empty() ||
            venue_positions.find(asset) !=
                venue_positions.end()) {
            report.issues.push_back({
                ReconciliationIssueKind::UnexpectedVenuePosition,
                asset,
                0,
                {},
                "unique position",
                "duplicate/invalid position",
                "venue position snapshot is not uniquely keyed by canonical asset"
            });
            continue;
        }
        venue_positions.emplace(
            asset, position);
    }

    for (const auto& pair :
         local.ledger.positions) {
        const auto& expected =
            pair.second;

        if (expected.signed_quantity_units == 0)
            continue;

        const auto it =
            venue_positions.find(pair.first);

        if (it ==
            venue_positions.end()) {
            report.issues.push_back({
                ReconciliationIssueKind::PositionMissingOnVenue,
                pair.first,
                0,
                {},
                formatSignedScaled(
                    expected.signed_quantity_units,
                    defaultPerpetualRules().size_scale),
                "0",
                "local ledger position is absent from venue snapshot"
            });
            continue;
        }

        const Position& actual =
            it->second;

        if (!sameInstrument(
                expected.instrument,
                actual.instrument)) {
            report.issues.push_back({
                ReconciliationIssueKind::PositionIdentityMismatch,
                pair.first,
                0,
                {},
                expected.instrument.venue_asset_id,
                actual.instrument.venue_asset_id,
                "local and venue position identities differ"
            });
        }

        std::int64_t venue_qty = 0;
        if (!doubleToSignedUnitsExact(
                actual.signed_quantity,
                defaultPerpetualRules().size_scale,
                &venue_qty) ||
            venue_qty !=
                expected.signed_quantity_units) {
            report.issues.push_back({
                ReconciliationIssueKind::PositionQuantityMismatch,
                pair.first,
                0,
                {},
                formatSignedScaled(
                    expected.signed_quantity_units,
                    defaultPerpetualRules().size_scale),
                doubleText(
                    actual.signed_quantity),
                "local fill-derived quantity differs from venue position"
            });
        }

        std::uint64_t venue_entry = 0U;
        if (!doubleToScaledUnitsExact(
                actual.entry_price,
                defaultPerpetualRules().price_scale,
                &venue_entry) ||
            venue_entry !=
                expected.average_entry_price_units) {
            report.issues.push_back({
                ReconciliationIssueKind::PositionEntryPriceMismatch,
                pair.first,
                0,
                {},
                formatUnsignedScaled(
                    expected.average_entry_price_units,
                    defaultPerpetualRules().price_scale),
                doubleText(
                    actual.entry_price),
                "local fill-derived average entry differs from venue position"
            });
        }

        venue_positions.erase(it);
    }

    for (const auto& pair :
         venue_positions) {
        if (std::abs(
                pair.second.signed_quantity) >
            0.0) {
            report.issues.push_back({
                ReconciliationIssueKind::UnexpectedVenuePosition,
                pair.first,
                0,
                {},
                "0",
                doubleText(
                    pair.second.signed_quantity),
                "venue has a non-zero position absent from local ledger"
            });
        }
    }
}

void MockReconciliation::compareOpenOrders(
        const LocalExpectedState& local,
        const UserStateSnapshot& venue,
        ReconciliationReport& report)
{
    std::map<OrderID, OpenOrder>
        venue_orders;

    for (const auto& order :
         venue.open_orders.orders) {
        if (venue_orders.find(
                order.local_order_id) !=
            venue_orders.end()) {
            report.issues.push_back({
                ReconciliationIssueKind::UnexpectedVenueOpenOrder,
                order.instrument.market.canonical_asset,
                order.local_order_id,
                {},
                "unique open order",
                "duplicate local_order_id",
                "venue open-order snapshot contains duplicate local order id"
            });
            continue;
        }
        venue_orders.emplace(
            order.local_order_id,
            order);
    }

    for (const auto& pair :
         local.open_orders) {
        const auto it =
            venue_orders.find(pair.first);

        if (it ==
            venue_orders.end()) {
            report.issues.push_back({
                ReconciliationIssueKind::OpenOrderMissingOnVenue,
                pair.second.instrument.market.canonical_asset,
                pair.first,
                {},
                "present",
                "missing",
                "local open order is absent from venue snapshot"
            });
            continue;
        }

        const ExpectedOpenOrder& expected =
            pair.second;
        const OpenOrder& actual =
            it->second;

        if (!sameInstrument(
                expected.instrument,
                actual.instrument)) {
            report.issues.push_back({
                ReconciliationIssueKind::OpenOrderIdentityMismatch,
                expected.instrument.market.canonical_asset,
                pair.first,
                {},
                expected.instrument.venue_asset_id,
                actual.instrument.venue_asset_id,
                "local and venue open-order identities differ"
            });
        }

        if (expected.status !=
            actual.status) {
            report.issues.push_back({
                ReconciliationIssueKind::OpenOrderStatusMismatch,
                expected.instrument.market.canonical_asset,
                pair.first,
                {},
                VenueContracts::V1::toString(expected.status),
                VenueContracts::V1::toString(actual.status),
                "local and venue open-order statuses differ"
            });
        }

        const RuleProfile* rules =
            rulesForAsset(
                expected.instrument.market.canonical_asset);

        if (rules == nullptr) {
            report.issues.push_back({
                ReconciliationIssueKind::OpenOrderIdentityMismatch,
                expected.instrument.market.canonical_asset,
                pair.first,
                {},
                "valid rules",
                "missing",
                "open-order rule profile unavailable"
            });
        } else {
            std::uint64_t venue_cumulative = 0U;
            std::uint64_t venue_quantity = 0U;

            const bool numeric_ok =
                doubleToScaledUnitsExact(
                    actual.cumulative_filled_quantity,
                    rules->size_scale,
                    &venue_cumulative) &&
                doubleToScaledUnitsExact(
                    actual.quantity,
                    rules->size_scale,
                    &venue_quantity) &&
                venue_quantity >=
                    venue_cumulative;

            const std::uint64_t venue_remaining =
                numeric_ok
                ? venue_quantity -
                    venue_cumulative
                : 0U;

            if (!numeric_ok ||
                venue_cumulative !=
                    expected.cumulative_filled_units ||
                venue_remaining !=
                    expected.remaining_units) {
                report.issues.push_back({
                    ReconciliationIssueKind::OpenOrderQuantityMismatch,
                    expected.instrument.market.canonical_asset,
                    pair.first,
                    {},
                    std::to_string(
                        expected.cumulative_filled_units) +
                        "/" +
                        std::to_string(
                            expected.remaining_units),
                    numeric_ok
                        ? std::to_string(
                            venue_cumulative) +
                            "/" +
                            std::to_string(
                                venue_remaining)
                        : "<invalid>",
                    "open-order cumulative/remaining quantities differ"
                });
            }
        }

        if ((!expected.native_references.native_order_id.empty() &&
             expected.native_references.native_order_id !=
                 actual.native_references.native_order_id) ||
            (!expected.native_references.native_client_order_id.empty() &&
             expected.native_references.native_client_order_id !=
                 actual.native_references.native_client_order_id)) {
            report.issues.push_back({
                ReconciliationIssueKind::OpenOrderNativeIdentityMismatch,
                expected.instrument.market.canonical_asset,
                pair.first,
                {},
                expected.native_references.native_order_id,
                actual.native_references.native_order_id,
                "open-order native identity differs"
            });
        }

        venue_orders.erase(it);
    }

    for (const auto& pair :
         venue_orders) {
        report.issues.push_back({
            ReconciliationIssueKind::UnexpectedVenueOpenOrder,
            pair.second.instrument.market.canonical_asset,
            pair.first,
            {},
            "missing",
            "present",
            "venue has an open order absent from local expected state"
        });
    }
}

void MockReconciliation::compareFills(
        const LocalExpectedState& local,
        const UserStateSnapshot& venue,
        ReconciliationReport& report)
{
    std::map<std::string, Fill>
        venue_fills;

    for (const auto& fill :
         venue.fills.fills) {
        const std::string id =
            fill.native_references.native_fill_id;

        if (id.empty() ||
            venue_fills.find(id) !=
                venue_fills.end()) {
            report.issues.push_back({
                ReconciliationIssueKind::UnexpectedVenueFill,
                fill.instrument.market.canonical_asset,
                fill.local_order_id,
                id,
                "unique fill id",
                "duplicate/invalid fill id",
                "venue fill snapshot contains duplicate/invalid fill identity"
            });
            continue;
        }

        venue_fills.emplace(
            id, fill);
    }

    for (const auto& expected :
         local.ledger.fills) {
        const std::string id =
            expected.native_references.native_fill_id;
        const auto it =
            venue_fills.find(id);

        if (it ==
            venue_fills.end()) {
            report.issues.push_back({
                ReconciliationIssueKind::FillMissingOnVenue,
                expected.instrument.market.canonical_asset,
                expected.local_order_id,
                id,
                "present",
                "missing",
                "local canonical fill is absent from venue fill snapshot"
            });
            continue;
        }

        if (!sameFill(
                expected,
                it->second)) {
            report.issues.push_back({
                ReconciliationIssueKind::FillMismatch,
                expected.instrument.market.canonical_asset,
                expected.local_order_id,
                id,
                fillSummary(expected),
                fillSummary(it->second),
                "local and venue fill economics/identity differ"
            });
        }

        venue_fills.erase(it);
    }

    for (const auto& pair :
         venue_fills) {
        report.issues.push_back({
            ReconciliationIssueKind::UnexpectedVenueFill,
            pair.second.instrument.market.canonical_asset,
            pair.second.local_order_id,
            pair.first,
            "missing",
            "present",
            "venue has a fill absent from local canonical stream"
        });
    }

    const Balance* settlement = nullptr;
    for (const auto& balance :
         venue.account.balances) {
        if (balance.asset == "USD") {
            settlement = &balance;
            break;
        }
    }

    if (settlement != nullptr) {
        std::int64_t venue_cash = 0;
        if (!doubleToSignedUnitsExact(
                settlement->total,
                kMockMoneyScale,
                &venue_cash) ||
            venue_cash !=
                local.ledger.settled_cash_units) {
            report.issues.push_back({
                ReconciliationIssueKind::LedgerCashMismatch,
                "USD",
                0,
                {},
                formatSignedScaled(
                    local.ledger.settled_cash_units,
                    kMockMoneyScale),
                doubleText(
                    settlement->total),
                "ledger final cash is not in parity with venue account cash"
            });
        }
    }
}

bool MockReconciliation::sameFill(
        const Fill& a,
        const Fill& b)
{
    if (!sameInstrument(
            a.instrument,
            b.instrument) ||
        a.local_order_id !=
            b.local_order_id ||
        a.strategy_id !=
            b.strategy_id ||
        a.timestamp !=
            b.timestamp ||
        a.side !=
            b.side ||
        a.native_references.native_order_id !=
            b.native_references.native_order_id ||
        a.native_references.native_fill_id !=
            b.native_references.native_fill_id ||
        a.native_references.native_client_order_id !=
            b.native_references.native_client_order_id)
        return false;

    const RuleProfile* rules =
        rulesForAsset(
            a.instrument.market.canonical_asset);
    if (rules == nullptr)
        return false;

    std::uint64_t aq = 0U;
    std::uint64_t bq = 0U;
    std::uint64_t ap = 0U;
    std::uint64_t bp = 0U;

    return
        doubleToScaledUnitsExact(
            a.quantity,
            rules->size_scale,
            &aq) &&
        doubleToScaledUnitsExact(
            b.quantity,
            rules->size_scale,
            &bq) &&
        doubleToScaledUnitsExact(
            a.price,
            rules->price_scale,
            &ap) &&
        doubleToScaledUnitsExact(
            b.price,
            rules->price_scale,
            &bp) &&
        aq == bq &&
        ap == bp;
}

const RuleProfile* MockReconciliation::rulesForAsset(
        const std::string& asset)
{
    const CatalogEntry* entry =
        findByCanonicalAssetExact(asset);
    return entry == nullptr
        ? nullptr
        : rulesForExact(*entry);
}

std::string MockReconciliation::doubleText(double value)
{
    char buffer[128];
    const auto result = std::to_chars(
        buffer,
        buffer + sizeof(buffer),
        value,
        std::chars_format::general);
    return result.ec == std::errc()
        ? std::string(buffer, result.ptr)
        : std::string("<invalid>");
}

std::string MockReconciliation::formatUnsignedScaled(
        std::uint64_t units,
        std::size_t scale)
{
    if (units >
        static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max()))
        return "<overflow>";
    return formatSignedScaled(
        static_cast<std::int64_t>(units),
        scale);
}

std::string MockReconciliation::fillSummary(
        const Fill& fill)
{
    std::ostringstream out;
    out << fill.native_references.native_fill_id
        << '|'
        << fill.local_order_id
        << '|'
        << fill.timestamp
        << '|'
        << static_cast<int>(fill.side)
        << '|'
        << doubleText(fill.quantity)
        << '|'
        << doubleText(fill.price);
    return out.str();
}

} // namespace MockVenue
