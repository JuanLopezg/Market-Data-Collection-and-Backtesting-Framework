#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "venue_account.h"
#include "venue_events.h"
#include "decimal.h"
#include "orders.h"
#include "catalog.h"
#include "rules.h"

// Maintain MOCK cash, positions, margin and accounting state from canonical
// fills and accounting events.
//
// This component owns economic account state. Order admission and matching live in
// separate MOCK components so lifecycle decisions cannot silently mutate balances.

namespace MockVenue {

using VenueContracts::V1::AccountingEvent;
using VenueContracts::V1::AccountingEventType;
using VenueContracts::V1::AccountSnapshot;
using VenueContracts::V1::Balance;
using VenueContracts::V1::Event;
using VenueContracts::V1::Fill;
using VenueContracts::V1::MarketIdentity;
using VenueContracts::V1::NativeReferences;
using VenueContracts::V1::OpenOrder;
using VenueContracts::V1::OpenOrdersSnapshot;
using VenueContracts::V1::OrderLifecycleStatus;
using VenueContracts::V1::Position;
using VenueContracts::V1::Side;

struct MockAccountingConfig {
    std::string settlement_asset = "USD";
    std::string initial_cash = "100000.00";
    std::uint32_t fee_ppm = 400U; // Parts per million: 400 is 0.04% of fill notional.
    unsigned int default_leverage = 1U;

    bool valid() const
    {
        std::int64_t cash = 0;
        return !settlement_asset.empty() &&
               parseSignedDecimalToScale(
                   initial_cash,
                   kMockMoneyScale,
                   &cash) &&
               cash >= 0 &&
               fee_ppm <= 1000000U &&
               default_leverage > 0U &&
               default_leverage <= defaultPerpetualRules().max_leverage;
    }
};

enum class FillApplyResult {
    Applied = 0,
    DuplicateIgnored,
    InvalidRejected,
    IdentityConflictUnsafe
};

enum class ExternalAccountingApplyResult {
    Applied = 0,
    DuplicateIgnored,
    InvalidRejected,
    IdentityConflictUnsafe
};

struct MarginState {
    Timestamp timestamp = 0;
    bool accounting_safe = true;
    double settled_cash = 0.0;
    double realized_pnl = 0.0;
    double unrealized_pnl = 0.0;
    double equity = 0.0;
    double gross_exposure = 0.0;
    double margin_used = 0.0;
    double available_margin = 0.0;
};

struct HistoricalOrderRecord {
    OpenOrder order;
    bool terminal = false;
};

// The header exposes the account model and its state. The implementation lives in
// the matching .cpp so callers can understand the contract without reading the
// fill-accounting and margin calculations.
class MockAccount {
public:
    using EventHandler = std::function<void(const Event&)>;

    explicit MockAccount(
        MockAccountingConfig config = {});

    void setEventHandler(EventHandler handler)
    {
        handler_ = std::move(handler);
    }

    const std::vector<Event>& emittedEvents() const
    {
        return emitted_events_;
    }

    const MockAccountingConfig& config() const
    {
        return config_;
    }

    void clearEmittedEvents()
    {
        emitted_events_.clear();
    }

    bool accountingSafe() const
    {
        return accounting_safe_;
    }

    std::int64_t settledCashUnits() const { return cash_units_; }
    std::int64_t realizedPnlUnits() const { return realized_pnl_units_; }
    std::int64_t feesPaidUnits() const { return fees_paid_units_; }
    std::int64_t rebatesUnits() const { return rebates_units_; }
    std::int64_t fundingUnits() const { return funding_units_; }

    bool setLeverageExact(
        const std::string& canonical_asset,
        unsigned int leverage);

    unsigned int leverageFor(
        const std::string& canonical_asset) const;

    FillApplyResult consume(const Event& event);

    FillApplyResult applyFill(const Fill& fill);

    ExternalAccountingApplyResult postRebate(
        const std::string& canonical_asset,
        Timestamp event_time,
        const std::string& positive_amount,
        const std::string& event_id);

    ExternalAccountingApplyResult postFundingPayment(
        const std::string& canonical_asset,
        Timestamp event_time,
        const std::string& signed_amount,
        const std::string& event_id);

    bool markToMarket(
        const std::string& canonical_asset,
        Timestamp event_time,
        double mark_price);

    AccountSnapshot accountSnapshot(Timestamp timestamp) const;

    MarginState marginState(Timestamp timestamp) const;

    OpenOrdersSnapshot openOrdersSnapshot(
        const MockOrders& lifecycle,
        Timestamp timestamp) const;

    std::vector<HistoricalOrderRecord> historicalOrders(
        const MockOrders& lifecycle) const;

    std::string economicFingerprint() const;

private:
    struct PositionState {
        bool initialized = false;
        VenueContracts::V1::InstrumentIdentity instrument;
        std::int64_t signed_quantity_units = 0;
        std::uint64_t average_entry_price_units = 0U;
        std::uint64_t mark_price_units = 0U;
        Timestamp last_event_time = 0;
    };

    struct DerivedTotals {
        std::int64_t unrealized_pnl_units = 0;
        std::int64_t equity_units = 0;
        std::uint64_t gross_exposure_units = 0U;
        std::uint64_t margin_used_units = 0U;
        std::int64_t available_margin_units = 0;
    };

    MockAccountingConfig config_;
    std::int64_t cash_units_ = 0; // Settlement currency multiplied by 10^kMockMoneyScale.
    std::int64_t realized_pnl_units_ = 0;
    std::int64_t fees_paid_units_ = 0;
    std::int64_t rebates_units_ = 0;
    std::int64_t funding_units_ = 0;
    bool accounting_safe_ = true;

    std::map<std::string, PositionState> positions_;
    std::map<std::string, unsigned int> leverage_by_asset_;
    std::map<std::string, std::string> fill_fingerprints_;
    std::map<std::string, std::string> external_event_fingerprints_;

    EventHandler handler_;
    std::vector<Event> emitted_events_;

    static bool checkedAddSigned(
        std::int64_t a,
        std::int64_t b,
        std::int64_t* out);

    static std::uint64_t absQuantityUnits(std::int64_t value);

    static int signOf(std::int64_t value);

    static double unsignedScaledToDouble(
        std::uint64_t units,
        std::size_t scale);

    bool validateFill(
        const Fill& fill,
        const CatalogEntry** out_entry,
        std::uint64_t* out_quantity_units,
        std::uint64_t* out_price_units) const;

    static bool sameInstrument(
        const VenueContracts::V1::InstrumentIdentity& a,
        const VenueContracts::V1::InstrumentIdentity& b);

    static std::string fillFingerprint(
        const Fill& fill,
        std::uint64_t quantity_units,
        std::uint64_t price_units);

    bool applyPositionDelta(
        PositionState& state,
        std::int64_t signed_delta,
        std::uint64_t fill_price_units,
        std::int64_t* out_realized_delta);

    std::int64_t unrealizedPnlUnits(
        const PositionState& state) const;

    std::uint64_t effectiveMarkPriceUnits(
        const PositionState& state) const;

    DerivedTotals deriveTotals() const;

    ExternalAccountingApplyResult postExternalAccounting(
        AccountingEventType type,
        const std::string& canonical_asset,
        Timestamp event_time,
        std::int64_t amount_units,
        const std::string& event_id);

    static OpenOrder toOpenOrder(const StoredOrder& stored);

    template <typename T>
    void emit(const T& value)
    {
        Event event = value;
        emitted_events_.push_back(event);
        if (handler_)
            handler_(event);
    }
};

} // namespace MockVenue
