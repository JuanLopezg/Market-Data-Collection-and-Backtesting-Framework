#pragma once

#include <initializer_list>
#include <set>
#include <vector>

/**************************************************************************************
 * Purpose : Stable capability negotiation for heterogeneous execution venues.
 *
 * A caller MUST query capabilities instead of branching on venue identity. Requiring an
 * unsupported capability blocks that route; it never implies fallback to another venue.
 **************************************************************************************/
namespace VenueContracts {
namespace V1 {

enum class Capability {
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
    RateLimitIntrospection,

    // Capabilities used by status, trigger-order and accounting workflows.
    OrderStatusQuery,
    TriggerOrders,
    FeeAccounting,
    FundingAccounting
};

class CapabilitySet {
public:
    CapabilitySet() = default;

    CapabilitySet(std::initializer_list<Capability> capabilities)
        : values_(capabilities)
    {
    }

    bool supports(Capability capability) const
    {
        return values_.find(capability) != values_.end();
    }

    bool supportsAll(std::initializer_list<Capability> capabilities) const
    {
        for (const auto capability : capabilities) {
            if (!supports(capability))
                return false;
        }
        return true;
    }

    std::vector<Capability> missing(std::initializer_list<Capability> capabilities) const
    {
        std::vector<Capability> result;
        for (const auto capability : capabilities) {
            if (!supports(capability))
                result.push_back(capability);
        }
        return result;
    }

    void add(Capability capability)
    {
        values_.insert(capability);
    }

    const std::set<Capability>& values() const
    {
        return values_;
    }

private:
    std::set<Capability> values_;
};

inline const char* toString(Capability capability)
{
    switch (capability) {
    case Capability::MarketMetadata: return "MARKET_METADATA";
    case Capability::TradingRules: return "TRADING_RULES";
    case Capability::SubmitOrder: return "SUBMIT_ORDER";
    case Capability::CancelOrder: return "CANCEL_ORDER";
    case Capability::ModifyOrder: return "MODIFY_ORDER";
    case Capability::ClientOrderId: return "CLIENT_ORDER_ID";
    case Capability::IdempotentSubmit: return "IDEMPOTENT_SUBMIT";
    case Capability::TimeInForceGtc: return "TIF_GTC";
    case Capability::TimeInForceIoc: return "TIF_IOC";
    case Capability::PostOnly: return "POST_ONLY";
    case Capability::ReduceOnly: return "REDUCE_ONLY";
    case Capability::BatchActions: return "BATCH_ACTIONS";
    case Capability::Leverage: return "LEVERAGE";
    case Capability::MarginState: return "MARGIN_STATE";
    case Capability::AccountSnapshot: return "ACCOUNT_SNAPSHOT";
    case Capability::OpenOrders: return "OPEN_ORDERS";
    case Capability::HistoricalOrders: return "HISTORICAL_ORDERS";
    case Capability::Fills: return "FILLS";
    case Capability::UserStream: return "USER_STREAM";
    case Capability::SnapshotBackfill: return "SNAPSHOT_BACKFILL";
    case Capability::DeadMansSwitch: return "DEAD_MANS_SWITCH";
    case Capability::RateLimitIntrospection: return "RATE_LIMIT_INTROSPECTION";
    case Capability::OrderStatusQuery: return "ORDER_STATUS_QUERY";
    case Capability::TriggerOrders: return "TRIGGER_ORDERS";
    case Capability::FeeAccounting: return "FEE_ACCOUNTING";
    case Capability::FundingAccounting: return "FUNDING_ACCOUNTING";
    }
    return "UNKNOWN_CAPABILITY";
}

} // namespace V1
} // namespace VenueContracts
