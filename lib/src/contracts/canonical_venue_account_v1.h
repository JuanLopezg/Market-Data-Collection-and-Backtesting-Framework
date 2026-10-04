#pragma once

#include <cmath>
#include <cctype>
#include <optional>
#include <string>
#include <vector>

#include "data_types.h"
#include "canonical_venue_identity_v1.h"
#include "canonical_venue_orders_v1.h"

/**************************************************************************************
 * Header  : canonical_venue_account_v1.h
 * Step    : 48 — Canonical Multi-Venue Adapter Contract v1
 * Purpose : Venue-neutral fills, account/open-order snapshots, recovery and accounting
 **************************************************************************************/
namespace VenueContracts {
namespace V1 {

inline bool isDecimalString(const std::string& value)
{
    if (value.empty())
        return false;

    std::size_t i = 0;
    if (value[i] == '-')
        ++i;
    if (i == value.size())
        return false;

    bool saw_digit = false;
    bool saw_dot = false;
    for (; i < value.size(); ++i) {
        const char ch = value[i];
        if (ch == '.') {
            if (saw_dot)
                return false;
            saw_dot = true;
            continue;
        }
        if (!std::isdigit(static_cast<unsigned char>(ch)))
            return false;
        saw_digit = true;
    }

    return saw_digit && value.front() != '.' && value.back() != '.';
}

struct Fill {
    VenueContext venue;
    InstrumentIdentity instrument;

    OrderID local_order_id = 0;
    StrategyID strategy_id = 0;
    Timestamp timestamp = 0;

    Side side = Side::Buy;
    double quantity = 0.0;
    double price = 0.0;

    NativeReferences native_references;

    bool valid() const
    {
        return venue.valid() &&
               instrument.valid() &&
               local_order_id != 0 &&
               !native_references.native_fill_id.empty() &&
               std::isfinite(quantity) && quantity > 0.0 &&
               std::isfinite(price) && price > 0.0;
    }
};

enum class AccountingEventType {
    TradingFee = 0,
    Rebate,
    FundingPayment
};

struct AccountingEvent {
    VenueContext venue;
    Timestamp timestamp = 0;
    AccountingEventType type = AccountingEventType::TradingFee;

    // Signed account-centric amount:
    //   positive = increases account value
    //   negative = decreases account value
    std::string amount;
    std::string settlement_asset;

    std::optional<MarketIdentity> market;
    NativeReferences native_references;
    std::string correlation_id;

    bool valid() const
    {
        return venue.valid() &&
               isDecimalString(amount) &&
               !settlement_asset.empty() &&
               (!market || market->valid());
    }
};

struct Balance {
    std::string asset;
    double total = 0.0;
    double available = 0.0;

    bool valid() const
    {
        return !asset.empty() &&
               std::isfinite(total) &&
               std::isfinite(available);
    }
};

struct Position {
    InstrumentIdentity instrument;
    double signed_quantity = 0.0;
    double entry_price = 0.0;
    double mark_price = 0.0;
    double unrealized_pnl = 0.0;
    double leverage = 0.0;

    bool valid() const
    {
        return instrument.valid() &&
               std::isfinite(signed_quantity) &&
               std::isfinite(entry_price) &&
               entry_price >= 0.0 &&
               std::isfinite(mark_price) &&
               mark_price >= 0.0 &&
               std::isfinite(unrealized_pnl) &&
               std::isfinite(leverage) &&
               leverage >= 0.0;
    }
};

struct AccountSnapshot {
    VenueContext venue;
    Timestamp timestamp = 0;

    double equity = 0.0;
    double margin_used = 0.0;

    std::vector<Balance> balances;
    std::vector<Position> positions;

    bool valid() const
    {
        if (!venue.valid() ||
            !std::isfinite(equity) ||
            !std::isfinite(margin_used) ||
            margin_used < 0.0)
            return false;

        for (const auto& balance : balances) {
            if (!balance.valid())
                return false;
        }
        for (const auto& position : positions) {
            if (!position.valid())
                return false;
        }
        return true;
    }
};

struct OpenOrder {
    OrderID local_order_id = 0;
    InstrumentIdentity instrument;
    Side side = Side::Buy;
    double quantity = 0.0;
    double cumulative_filled_quantity = 0.0;
    double limit_price = 0.0;
    OrderLifecycleStatus status = OrderLifecycleStatus::UnknownRequiresReconciliation;
    NativeReferences native_references;
    std::string native_status;

    bool valid() const
    {
        return local_order_id != 0 &&
               instrument.valid() &&
               std::isfinite(quantity) && quantity > 0.0 &&
               std::isfinite(cumulative_filled_quantity) &&
               cumulative_filled_quantity >= 0.0 &&
               cumulative_filled_quantity <= quantity &&
               std::isfinite(limit_price) && limit_price > 0.0;
    }
};

struct OpenOrdersSnapshot {
    VenueContext venue;
    Timestamp timestamp = 0;
    std::vector<OpenOrder> orders;

    bool valid() const
    {
        if (!venue.valid())
            return false;
        for (const auto& order : orders) {
            if (!order.valid())
                return false;
        }
        return true;
    }
};

struct FillBatch {
    VenueContext venue;
    Timestamp produced_at = 0;
    bool is_snapshot = false;
    std::vector<Fill> fills;

    // Opaque adapter-owned continuation token. Empty means no continuation supplied.
    std::string next_cursor;

    bool valid() const
    {
        if (!venue.valid())
            return false;
        for (const auto& fill : fills) {
            if (!fill.valid())
                return false;
        }
        return true;
    }
};

struct AccountSnapshotRequest {
    RequestIdentity request;

    bool valid() const { return request.valid(); }
};

struct OpenOrdersRequest {
    RequestIdentity request;

    bool valid() const { return request.valid(); }
};

struct OrderStatusRequest {
    RequestIdentity request;
    OrderLocator order;

    bool valid() const { return request.valid() && order.valid(); }
};

struct FillBackfillRequest {
    RequestIdentity request;
    Timestamp from_inclusive = 0;
    Timestamp to_inclusive = 0;
    std::string cursor;

    bool valid() const
    {
        return request.valid() && from_inclusive <= to_inclusive;
    }
};

inline const char* toString(AccountingEventType type)
{
    switch (type) {
    case AccountingEventType::TradingFee: return "TRADING_FEE";
    case AccountingEventType::Rebate: return "REBATE";
    case AccountingEventType::FundingPayment: return "FUNDING_PAYMENT";
    }
    return "TRADING_FEE";
}

} // namespace V1
} // namespace VenueContracts
