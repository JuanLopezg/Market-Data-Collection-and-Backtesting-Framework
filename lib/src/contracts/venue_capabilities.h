#pragma once

#include <initializer_list>
#include <set>

/**************************************************************************************
 * Header  : venue_capabilities.h
 * Step    : 47A — Canonical Multi-Exchange Architecture Baseline
 * Purpose : Explicit feature negotiation for heterogeneous execution venues
 *
 * The core must query capabilities rather than assume every venue supports the same
 * order lifecycle, margin model, user stream or recovery primitives.
 **************************************************************************************/
namespace VenueContracts {

enum class VenueCapability {
    MarketMetadata = 0,
    TradingRules,
    SubmitOrder,
    CancelOrder,
    ModifyOrder,
    ClientOrderId,
    IdempotentSubmit,
    TimeInForceGtc,
    TimeInForceIoc,
    PostOnly,
    ReduceOnly,
    BatchActions,
    Leverage,
    MarginState,
    AccountSnapshot,
    OpenOrders,
    HistoricalOrders,
    Fills,
    UserStream,
    SnapshotBackfill,
    DeadMansSwitch,
    RateLimitIntrospection
};

class VenueCapabilitySet {
public:
    VenueCapabilitySet() = default;

    VenueCapabilitySet(std::initializer_list<VenueCapability> capabilities)
        : values_(capabilities)
    {
    }

    bool supports(VenueCapability capability) const
    {
        return values_.find(capability) != values_.end();
    }

    bool supportsAll(std::initializer_list<VenueCapability> capabilities) const
    {
        for (const auto capability : capabilities) {
            if (!supports(capability))
                return false;
        }
        return true;
    }

    void add(VenueCapability capability)
    {
        values_.insert(capability);
    }

    const std::set<VenueCapability>& values() const
    {
        return values_;
    }

private:
    std::set<VenueCapability> values_;
};

inline const char* toString(VenueCapability capability)
{
    switch (capability) {
    case VenueCapability::MarketMetadata: return "MARKET_METADATA";
    case VenueCapability::TradingRules: return "TRADING_RULES";
    case VenueCapability::SubmitOrder: return "SUBMIT_ORDER";
    case VenueCapability::CancelOrder: return "CANCEL_ORDER";
    case VenueCapability::ModifyOrder: return "MODIFY_ORDER";
    case VenueCapability::ClientOrderId: return "CLIENT_ORDER_ID";
    case VenueCapability::IdempotentSubmit: return "IDEMPOTENT_SUBMIT";
    case VenueCapability::TimeInForceGtc: return "TIF_GTC";
    case VenueCapability::TimeInForceIoc: return "TIF_IOC";
    case VenueCapability::PostOnly: return "POST_ONLY";
    case VenueCapability::ReduceOnly: return "REDUCE_ONLY";
    case VenueCapability::BatchActions: return "BATCH_ACTIONS";
    case VenueCapability::Leverage: return "LEVERAGE";
    case VenueCapability::MarginState: return "MARGIN_STATE";
    case VenueCapability::AccountSnapshot: return "ACCOUNT_SNAPSHOT";
    case VenueCapability::OpenOrders: return "OPEN_ORDERS";
    case VenueCapability::HistoricalOrders: return "HISTORICAL_ORDERS";
    case VenueCapability::Fills: return "FILLS";
    case VenueCapability::UserStream: return "USER_STREAM";
    case VenueCapability::SnapshotBackfill: return "SNAPSHOT_BACKFILL";
    case VenueCapability::DeadMansSwitch: return "DEAD_MANS_SWITCH";
    case VenueCapability::RateLimitIntrospection: return "RATE_LIMIT_INTROSPECTION";
    }
    return "UNKNOWN_CAPABILITY";
}

} // namespace VenueContracts
