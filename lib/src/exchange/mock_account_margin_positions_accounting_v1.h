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

#include "canonical_venue_account_v1.h"
#include "canonical_venue_events_v1.h"
#include "mock_accounting_fixed_point_v1.h"
#include "mock_order_admission_lifecycle_v1.h"
#include "mock_venue_catalog_v1.h"
#include "mock_venue_rules_v1.h"

namespace MockVenueV1 {

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

struct MockAccountingConfigV1 {
    std::string settlement_asset = "USD";
    std::string initial_cash = "100000.00";
    std::uint32_t fee_ppm = 400U;
    unsigned int default_leverage = 1U;

    bool valid() const
    {
        std::int64_t cash = 0;
        return !settlement_asset.empty() &&
               parseSignedDecimalToScale(
                   initial_cash,
                   kMockMoneyScaleV1,
                   &cash) &&
               cash >= 0 &&
               fee_ppm <= 1000000U &&
               default_leverage > 0U &&
               default_leverage <= defaultPerpetualRules().max_leverage;
    }
};

enum class FillApplyResultV1 {
    Applied = 0,
    DuplicateIgnored,
    InvalidRejected,
    IdentityConflictUnsafe
};

enum class ExternalAccountingApplyResultV1 {
    Applied = 0,
    DuplicateIgnored,
    InvalidRejected,
    IdentityConflictUnsafe
};

struct MarginStateV1 {
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

struct HistoricalOrderRecordV1 {
    OpenOrder order;
    bool terminal = false;
};

class MockAccountMarginPositionsAccountingV1 {
public:
    using EventHandler = std::function<void(const Event&)>;

    explicit MockAccountMarginPositionsAccountingV1(
        MockAccountingConfigV1 config = {})
        : config_(std::move(config))
    {
        if (!config_.valid())
            throw std::invalid_argument("invalid MOCK Step52 accounting config");

        if (!parseSignedDecimalToScale(
                config_.initial_cash,
                kMockMoneyScaleV1,
                &cash_units_))
            throw std::invalid_argument("invalid MOCK initial cash");
    }

    void setEventHandler(EventHandler handler)
    {
        handler_ = std::move(handler);
    }

    const std::vector<Event>& emittedEvents() const
    {
        return emitted_events_;
    }

    const MockAccountingConfigV1& config() const
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
        unsigned int leverage)
    {
        const CatalogEntry* entry =
            findByCanonicalAssetExact(canonical_asset);
        const RuleProfile* rules =
            entry == nullptr ? nullptr : rulesForExact(*entry);

        if (entry == nullptr ||
            rules == nullptr ||
            !entry->enabled ||
            leverage == 0U ||
            leverage > rules->max_leverage)
            return false;

        leverage_by_asset_[canonical_asset] = leverage;
        return true;
    }

    unsigned int leverageFor(
        const std::string& canonical_asset) const
    {
        const auto it = leverage_by_asset_.find(canonical_asset);
        return it == leverage_by_asset_.end()
            ? config_.default_leverage
            : it->second;
    }

    FillApplyResultV1 consume(const Event& event)
    {
        if (!std::holds_alternative<Fill>(event))
            return FillApplyResultV1::InvalidRejected;
        return applyFill(std::get<Fill>(event));
    }

    FillApplyResultV1 applyFill(const Fill& fill)
    {
        std::uint64_t quantity_units = 0U;
        std::uint64_t price_units = 0U;
        const CatalogEntry* entry = nullptr;

        if (!validateFill(
                fill,
                &entry,
                &quantity_units,
                &price_units))
            return FillApplyResultV1::InvalidRejected;

        const std::string fingerprint =
            fillFingerprint(fill, quantity_units, price_units);
        const std::string& fill_id =
            fill.native_references.native_fill_id;

        const auto existing = fill_fingerprints_.find(fill_id);
        if (existing != fill_fingerprints_.end()) {
            if (existing->second == fingerprint)
                return FillApplyResultV1::DuplicateIgnored;
            accounting_safe_ = false;
            return FillApplyResultV1::IdentityConflictUnsafe;
        }

        if (quantity_units >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max()))
            return FillApplyResultV1::InvalidRejected;

        const std::int64_t signed_delta =
            fill.side == Side::Buy
            ? static_cast<std::int64_t>(quantity_units)
            : -static_cast<std::int64_t>(quantity_units);

        PositionState& state = positions_[entry->canonical_asset];
        if (!state.initialized) {
            state.instrument = entry->instrument();
            state.initialized = true;
        }

        std::int64_t realized_delta = 0;
        if (!applyPositionDelta(
                state,
                signed_delta,
                price_units,
                &realized_delta))
            return FillApplyResultV1::InvalidRejected;

        std::uint64_t notional_units = 0U;
        std::uint64_t fee_units_u = 0U;
        if (!moneyNotionalUnits(
                price_units,
                quantity_units,
                &notional_units) ||
            !ppmAmountUnits(
                notional_units,
                config_.fee_ppm,
                &fee_units_u) ||
            fee_units_u >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max()))
            return FillApplyResultV1::InvalidRejected;

        const std::int64_t fee_units =
            static_cast<std::int64_t>(fee_units_u);

        if (!checkedAddSigned(
                cash_units_,
                realized_delta,
                &cash_units_) ||
            !checkedAddSigned(
                realized_pnl_units_,
                realized_delta,
                &realized_pnl_units_) ||
            !checkedAddSigned(
                cash_units_,
                -fee_units,
                &cash_units_) ||
            !checkedAddSigned(
                fees_paid_units_,
                fee_units,
                &fees_paid_units_))
            throw std::overflow_error("MOCK Step52 accounting overflow");

        state.mark_price_units = price_units;
        state.last_event_time = fill.timestamp;

        fill_fingerprints_.emplace(fill_id, fingerprint);

        if (fee_units > 0) {
            AccountingEvent fee;
            fee.venue = context();
            fee.timestamp = fill.timestamp;
            fee.type = AccountingEventType::TradingFee;
            fee.amount = formatSignedScaled(
                -fee_units,
                kMockMoneyScaleV1);
            fee.settlement_asset = config_.settlement_asset;
            fee.market = fill.instrument.market;
            fee.native_references = fill.native_references;
            fee.correlation_id = "fee:" + fill_id;

            if (!fee.valid())
                throw std::runtime_error("generated MOCK fee event invalid");
            emit(fee);
        }

        return FillApplyResultV1::Applied;
    }

    ExternalAccountingApplyResultV1 postRebate(
        const std::string& canonical_asset,
        Timestamp event_time,
        const std::string& positive_amount,
        const std::string& event_id)
    {
        std::int64_t units = 0;
        if (!parseSignedDecimalToScale(
                positive_amount,
                kMockMoneyScaleV1,
                &units) ||
            units <= 0)
            return ExternalAccountingApplyResultV1::InvalidRejected;

        return postExternalAccounting(
            AccountingEventType::Rebate,
            canonical_asset,
            event_time,
            units,
            event_id);
    }

    ExternalAccountingApplyResultV1 postFundingPayment(
        const std::string& canonical_asset,
        Timestamp event_time,
        const std::string& signed_amount,
        const std::string& event_id)
    {
        std::int64_t units = 0;
        if (!parseSignedDecimalToScale(
                signed_amount,
                kMockMoneyScaleV1,
                &units))
            return ExternalAccountingApplyResultV1::InvalidRejected;

        return postExternalAccounting(
            AccountingEventType::FundingPayment,
            canonical_asset,
            event_time,
            units,
            event_id);
    }

    bool markToMarket(
        const std::string& canonical_asset,
        Timestamp event_time,
        double mark_price)
    {
        const CatalogEntry* entry =
            findByCanonicalAssetExact(canonical_asset);
        if (entry == nullptr || !entry->enabled)
            return false;

        std::uint64_t price_units = 0U;
        const RuleProfile* rules = rulesForExact(*entry);
        if (rules == nullptr ||
            !divisibleByIncrement(
                mark_price,
                rules->price_increment,
                rules->price_scale,
                &price_units))
            return false;

        PositionState& state = positions_[canonical_asset];
        if (!state.initialized) {
            state.instrument = entry->instrument();
            state.initialized = true;
        }

        state.mark_price_units = price_units;
        state.last_event_time = event_time;
        return true;
    }

    AccountSnapshot accountSnapshot(Timestamp timestamp) const
    {
        const DerivedTotals totals = deriveTotals();

        AccountSnapshot snapshot;
        snapshot.venue = context();
        snapshot.timestamp = timestamp;
        snapshot.equity = signedScaledToDouble(
            totals.equity_units,
            kMockMoneyScaleV1);
        snapshot.margin_used = signedScaledToDouble(
            static_cast<std::int64_t>(totals.margin_used_units),
            kMockMoneyScaleV1);

        Balance settlement;
        settlement.asset = config_.settlement_asset;
        settlement.total = signedScaledToDouble(
            cash_units_,
            kMockMoneyScaleV1);
        settlement.available = signedScaledToDouble(
            totals.available_margin_units,
            kMockMoneyScaleV1);
        snapshot.balances.push_back(settlement);

        for (const auto& pair : positions_) {
            const PositionState& state = pair.second;
            if (!state.initialized || state.signed_quantity_units == 0)
                continue;

            Position position;
            position.instrument = state.instrument;
            position.signed_quantity = signedScaledToDouble(
                state.signed_quantity_units,
                defaultPerpetualRules().size_scale);
            position.entry_price = unsignedScaledToDouble(
                state.average_entry_price_units,
                defaultPerpetualRules().price_scale);
            position.mark_price = unsignedScaledToDouble(
                effectiveMarkPriceUnits(state),
                defaultPerpetualRules().price_scale);
            position.unrealized_pnl = signedScaledToDouble(
                unrealizedPnlUnits(state),
                kMockMoneyScaleV1);
            position.leverage = static_cast<double>(
                leverageFor(pair.first));
            snapshot.positions.push_back(position);
        }

        if (!snapshot.valid())
            throw std::runtime_error("generated MOCK AccountSnapshot invalid");
        return snapshot;
    }

    MarginStateV1 marginState(Timestamp timestamp) const
    {
        const DerivedTotals totals = deriveTotals();

        MarginStateV1 state;
        state.timestamp = timestamp;
        state.accounting_safe = accounting_safe_;
        state.settled_cash = signedScaledToDouble(
            cash_units_, kMockMoneyScaleV1);
        state.realized_pnl = signedScaledToDouble(
            realized_pnl_units_, kMockMoneyScaleV1);
        state.unrealized_pnl = signedScaledToDouble(
            totals.unrealized_pnl_units, kMockMoneyScaleV1);
        state.equity = signedScaledToDouble(
            totals.equity_units, kMockMoneyScaleV1);
        state.gross_exposure = signedScaledToDouble(
            static_cast<std::int64_t>(totals.gross_exposure_units),
            kMockMoneyScaleV1);
        state.margin_used = signedScaledToDouble(
            static_cast<std::int64_t>(totals.margin_used_units),
            kMockMoneyScaleV1);
        state.available_margin = signedScaledToDouble(
            totals.available_margin_units, kMockMoneyScaleV1);
        return state;
    }

    OpenOrdersSnapshot openOrdersSnapshot(
        const MockOrderAdmissionLifecycleV1& lifecycle,
        Timestamp timestamp) const
    {
        OpenOrdersSnapshot snapshot;
        snapshot.venue = context();
        snapshot.timestamp = timestamp;

        for (const auto& pair : lifecycle.orders()) {
            const StoredOrder& stored = pair.second;
            if (!stored.active())
                continue;
            snapshot.orders.push_back(toOpenOrder(stored));
        }

        if (!snapshot.valid())
            throw std::runtime_error("generated MOCK OpenOrdersSnapshot invalid");
        return snapshot;
    }

    std::vector<HistoricalOrderRecordV1> historicalOrders(
        const MockOrderAdmissionLifecycleV1& lifecycle) const
    {
        std::vector<HistoricalOrderRecordV1> result;
        for (const auto& pair : lifecycle.orders()) {
            HistoricalOrderRecordV1 record;
            record.order = toOpenOrder(pair.second);
            record.terminal =
                VenueContracts::V1::isTerminal(pair.second.status);
            result.push_back(record);
        }
        return result;
    }

    std::string economicFingerprint() const
    {
        std::ostringstream out;
        out.imbue(std::locale::classic());

        out << "cash=" << cash_units_
            << "|realized=" << realized_pnl_units_
            << "|fees=" << fees_paid_units_
            << "|rebates=" << rebates_units_
            << "|funding=" << funding_units_
            << "|safe=" << (accounting_safe_ ? 1 : 0);

        for (const auto& pair : leverage_by_asset_)
            out << "|lev:" << pair.first << '=' << pair.second;

        for (const auto& pair : positions_) {
            const PositionState& p = pair.second;
            out << "|pos:" << pair.first
                << ':' << p.signed_quantity_units
                << ':' << p.average_entry_price_units
                << ':' << p.mark_price_units
                << ':' << p.last_event_time;
        }

        for (const auto& pair : fill_fingerprints_)
            out << "|fill:" << pair.first << '=' << pair.second;

        for (const auto& pair : external_event_fingerprints_)
            out << "|acct:" << pair.first << '=' << pair.second;

        const std::string payload = out.str();
        std::uint64_t hash = 1469598103934665603ULL;
        for (const unsigned char ch : payload) {
            hash ^= static_cast<std::uint64_t>(ch);
            hash *= 1099511628211ULL;
        }

        std::ostringstream hex;
        hex.imbue(std::locale::classic());
        hex << std::hex << std::setw(16) << std::setfill('0') << hash;
        return hex.str();
    }

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

    MockAccountingConfigV1 config_;
    std::int64_t cash_units_ = 0;
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
        std::int64_t* out)
    {
        if (out == nullptr)
            return false;
        if ((b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) ||
            (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b))
            return false;
        *out = a + b;
        return true;
    }

    static std::uint64_t absQuantityUnits(std::int64_t value)
    {
        if (value == std::numeric_limits<std::int64_t>::min())
            throw std::overflow_error("MOCK quantity magnitude overflow");
        return value < 0
            ? static_cast<std::uint64_t>(-value)
            : static_cast<std::uint64_t>(value);
    }

    static int signOf(std::int64_t value)
    {
        return value > 0 ? 1 : (value < 0 ? -1 : 0);
    }

    static double unsignedScaledToDouble(
        std::uint64_t units,
        std::size_t scale)
    {
        long double factor = 1.0L;
        for (std::size_t i = 0; i < scale; ++i)
            factor *= 10.0L;
        return static_cast<double>(
            static_cast<long double>(units) / factor);
    }

    bool validateFill(
        const Fill& fill,
        const CatalogEntry** out_entry,
        std::uint64_t* out_quantity_units,
        std::uint64_t* out_price_units) const
    {
        if (!fill.valid() ||
            fill.venue.venue_id != "MOCK" ||
            fill.venue.environment != VenueContracts::VenueEnvironment::Mock ||
            !fill.venue.custom_environment.empty())
            return false;

        const CatalogEntry* entry =
            findByCanonicalAssetExact(
                fill.instrument.market.canonical_asset);
        if (entry == nullptr ||
            !entry->enabled ||
            !sameInstrument(fill.instrument, entry->instrument()))
            return false;

        const RuleProfile* rules = rulesForExact(*entry);
        if (rules == nullptr || !rules->valid())
            return false;

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
                &price_units))
            return false;

        if (out_entry != nullptr)
            *out_entry = entry;
        if (out_quantity_units != nullptr)
            *out_quantity_units = quantity_units;
        if (out_price_units != nullptr)
            *out_price_units = price_units;
        return true;
    }

    static bool sameInstrument(
        const VenueContracts::V1::InstrumentIdentity& a,
        const VenueContracts::V1::InstrumentIdentity& b)
    {
        return a.market.canonical_asset == b.market.canonical_asset &&
               a.market.product_class == b.market.product_class &&
               a.market.quote_or_settlement_asset ==
                   b.market.quote_or_settlement_asset &&
               a.market.contract_variant == b.market.contract_variant &&
               a.venue_symbol == b.venue_symbol &&
               a.venue_asset_id == b.venue_asset_id;
    }

    static std::string fillFingerprint(
        const Fill& fill,
        std::uint64_t quantity_units,
        std::uint64_t price_units)
    {
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << fill.venue.venue_id << '|'
            << static_cast<int>(fill.venue.environment) << '|'
            << fill.instrument.market.canonical_asset << '|'
            << static_cast<int>(fill.instrument.market.product_class) << '|'
            << fill.instrument.venue_symbol << '|'
            << fill.instrument.venue_asset_id << '|'
            << fill.local_order_id << '|'
            << fill.strategy_id << '|'
            << fill.timestamp << '|'
            << static_cast<int>(fill.side) << '|'
            << quantity_units << '|'
            << price_units << '|'
            << fill.native_references.native_order_id << '|'
            << fill.native_references.native_fill_id << '|'
            << fill.native_references.native_client_order_id;
        return out.str();
    }

    bool applyPositionDelta(
        PositionState& state,
        std::int64_t signed_delta,
        std::uint64_t fill_price_units,
        std::int64_t* out_realized_delta)
    {
        if (out_realized_delta == nullptr || signed_delta == 0)
            return false;
        *out_realized_delta = 0;

        const std::int64_t old_q = state.signed_quantity_units;
        const int old_sign = signOf(old_q);
        const int delta_sign = signOf(signed_delta);
        const std::uint64_t delta_abs = absQuantityUnits(signed_delta);

        if (old_q == 0 || old_sign == delta_sign) {
            const std::uint64_t old_abs = absQuantityUnits(old_q);
            std::uint64_t new_average = 0U;
            if (!weightedAveragePriceUnits(
                    state.average_entry_price_units,
                    old_abs,
                    fill_price_units,
                    delta_abs,
                    &new_average))
                return false;

            std::int64_t new_q = 0;
            if (!checkedAddSigned(old_q, signed_delta, &new_q))
                return false;

            state.signed_quantity_units = new_q;
            state.average_entry_price_units = new_average;
            return true;
        }

        const std::uint64_t old_abs = absQuantityUnits(old_q);
        const std::uint64_t closing =
            std::min(old_abs, delta_abs);

        const std::int64_t price_difference =
            fill_price_units >= state.average_entry_price_units
            ? static_cast<std::int64_t>(
                fill_price_units - state.average_entry_price_units)
            : -static_cast<std::int64_t>(
                state.average_entry_price_units - fill_price_units);

        std::int64_t close_pnl = 0;
        if (!signedPriceQuantityPnlUnits(
                price_difference,
                closing,
                &close_pnl))
            return false;

        if (old_sign < 0)
            close_pnl = -close_pnl;

        std::int64_t new_q = 0;
        if (!checkedAddSigned(old_q, signed_delta, &new_q))
            return false;

        state.signed_quantity_units = new_q;
        *out_realized_delta = close_pnl;

        if (new_q == 0) {
            state.average_entry_price_units = 0U;
        } else if (signOf(new_q) != old_sign) {
            state.average_entry_price_units = fill_price_units;
        }
        // Partial close in the same original direction leaves average entry unchanged.

        return true;
    }

    std::int64_t unrealizedPnlUnits(
        const PositionState& state) const
    {
        if (state.signed_quantity_units == 0)
            return 0;

        const std::uint64_t mark =
            effectiveMarkPriceUnits(state);

        const std::int64_t difference =
            mark >= state.average_entry_price_units
            ? static_cast<std::int64_t>(
                mark - state.average_entry_price_units)
            : -static_cast<std::int64_t>(
                state.average_entry_price_units - mark);

        std::int64_t pnl = 0;
        if (!signedPriceQuantityPnlUnits(
                difference,
                absQuantityUnits(state.signed_quantity_units),
                &pnl))
            throw std::overflow_error("MOCK unrealized PnL overflow");

        return state.signed_quantity_units < 0 ? -pnl : pnl;
    }

    std::uint64_t effectiveMarkPriceUnits(
        const PositionState& state) const
    {
        return state.mark_price_units != 0U
            ? state.mark_price_units
            : state.average_entry_price_units;
    }

    DerivedTotals deriveTotals() const
    {
        DerivedTotals totals;
        totals.equity_units = cash_units_;

        for (const auto& pair : positions_) {
            const PositionState& p = pair.second;
            if (!p.initialized || p.signed_quantity_units == 0)
                continue;

            const std::int64_t unrealized =
                unrealizedPnlUnits(p);
            if (!checkedAddSigned(
                    totals.unrealized_pnl_units,
                    unrealized,
                    &totals.unrealized_pnl_units) ||
                !checkedAddSigned(
                    totals.equity_units,
                    unrealized,
                    &totals.equity_units))
                throw std::overflow_error("MOCK equity overflow");

            std::uint64_t exposure = 0U;
            if (!moneyNotionalUnits(
                    effectiveMarkPriceUnits(p),
                    absQuantityUnits(p.signed_quantity_units),
                    &exposure))
                throw std::overflow_error("MOCK exposure overflow");

            if (totals.gross_exposure_units >
                std::numeric_limits<std::uint64_t>::max() - exposure)
                throw std::overflow_error("MOCK gross exposure overflow");
            totals.gross_exposure_units += exposure;

            const unsigned int leverage =
                leverageFor(pair.first);
            const std::uint64_t margin =
                exposure / leverage +
                (exposure % leverage == 0U ? 0U : 1U);

            if (totals.margin_used_units >
                std::numeric_limits<std::uint64_t>::max() - margin)
                throw std::overflow_error("MOCK margin overflow");
            totals.margin_used_units += margin;
        }

        const std::int64_t margin_i =
            totals.margin_used_units >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max())
            ? std::numeric_limits<std::int64_t>::max()
            : static_cast<std::int64_t>(
                totals.margin_used_units);

        totals.available_margin_units =
            totals.equity_units > margin_i
            ? totals.equity_units - margin_i
            : 0;

        return totals;
    }

    ExternalAccountingApplyResultV1 postExternalAccounting(
        AccountingEventType type,
        const std::string& canonical_asset,
        Timestamp event_time,
        std::int64_t amount_units,
        const std::string& event_id)
    {
        if (event_id.empty() ||
            (type != AccountingEventType::Rebate &&
             type != AccountingEventType::FundingPayment))
            return ExternalAccountingApplyResultV1::InvalidRejected;

        const CatalogEntry* entry =
            findByCanonicalAssetExact(canonical_asset);
        if (entry == nullptr || !entry->enabled)
            return ExternalAccountingApplyResultV1::InvalidRejected;

        std::ostringstream fp;
        fp.imbue(std::locale::classic());
        fp << static_cast<int>(type) << '|'
           << canonical_asset << '|'
           << event_time << '|'
           << amount_units;

        const auto existing =
            external_event_fingerprints_.find(event_id);
        if (existing != external_event_fingerprints_.end()) {
            if (existing->second == fp.str())
                return ExternalAccountingApplyResultV1::DuplicateIgnored;
            accounting_safe_ = false;
            return ExternalAccountingApplyResultV1::IdentityConflictUnsafe;
        }

        std::int64_t new_cash = 0;
        if (!checkedAddSigned(cash_units_, amount_units, &new_cash))
            return ExternalAccountingApplyResultV1::InvalidRejected;

        if (type == AccountingEventType::Rebate) {
            if (amount_units <= 0)
                return ExternalAccountingApplyResultV1::InvalidRejected;
            if (!checkedAddSigned(
                    rebates_units_,
                    amount_units,
                    &rebates_units_))
                throw std::overflow_error("MOCK rebate overflow");
        } else {
            if (!checkedAddSigned(
                    funding_units_,
                    amount_units,
                    &funding_units_))
                throw std::overflow_error("MOCK funding overflow");
        }

        cash_units_ = new_cash;
        external_event_fingerprints_.emplace(
            event_id, fp.str());

        AccountingEvent event;
        event.venue = context();
        event.timestamp = event_time;
        event.type = type;
        event.amount = formatSignedScaled(
            amount_units,
            kMockMoneyScaleV1);
        event.settlement_asset = config_.settlement_asset;
        event.market = entry->instrument().market;
        event.correlation_id = event_id;

        if (!event.valid())
            throw std::runtime_error(
                "generated external MOCK accounting event invalid");

        emit(event);
        return ExternalAccountingApplyResultV1::Applied;
    }

    static OpenOrder toOpenOrder(
        const StoredOrder& stored)
    {
        OpenOrder order;
        order.local_order_id =
            stored.intent.local_order_id;
        order.instrument =
            stored.intent.instrument;
        order.side =
            stored.intent.side;
        order.quantity =
            stored.intent.quantity;
        order.cumulative_filled_quantity =
            stored.cumulative_filled_quantity;
        order.limit_price =
            stored.intent.limit_price;
        order.status =
            stored.status;
        order.native_references =
            stored.native_references;
        order.native_status =
            VenueContracts::V1::toString(stored.status);

        if (!order.valid())
            throw std::runtime_error(
                "generated MOCK historical/open order invalid");
        return order;
    }

    template <typename T>
    void emit(const T& value)
    {
        Event event = value;
        emitted_events_.push_back(event);
        if (handler_)
            handler_(event);
    }
};

} // namespace MockVenueV1
