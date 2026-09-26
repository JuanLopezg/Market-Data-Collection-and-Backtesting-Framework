#pragma once

#include <string>
#include <vector>


/**************************************************************************************
 * Canonical routing subjects. These strings are transport-neutral; a NATS adapter uses
 * them as subjects while tests/in-process adapters use the exact same contract names.
 **************************************************************************************/
namespace TransportSubjects {
inline constexpr const char* MARKET_DATA_RELEASE = "market.release.v1";
inline constexpr const char* MARKET_DATA_UPDATED = "market.data.updated.v1";
inline constexpr const char* MARKET_SLICE_CLOSED = "market.slice.closed.v1";
inline constexpr const char* MARKET_SLICE_SNAPSHOT = "market.slice.snapshot.v1";
inline constexpr const char* STRATEGY_INTENTS = "strategy.intents.v1";
inline constexpr const char* DECISION_BATCH = "decision.batch.v1";
inline constexpr const char* EXECUTION_PRICES = "execution.prices.v1";
inline constexpr const char* SUBMIT_ORDER = "execution.command.submit.v1";
inline constexpr const char* CANCEL_ORDER = "execution.command.cancel.v1";
inline constexpr const char* ORDER_UPDATE = "execution.event.order_update.v1";
inline constexpr const char* FILL = "execution.event.fill.v1";
inline constexpr const char* EXECUTION_EVENT_FILTER = "execution.event.>";
inline constexpr const char* ACCOUNT_SNAPSHOT = "execution.account.snapshot.v1";
inline constexpr const char* EXECUTION_CYCLE_COMPLETE = "execution.cycle.complete.v1";
inline constexpr const char* EXCHANGE_SNAPSHOT = "execution.exchange.snapshot.v1";
inline constexpr const char* EXCHANGE_SNAPSHOT_REQUEST = "execution.exchange.snapshot.request.v1";
inline constexpr const char* ORDER_PLANNING_REQUEST = "execution.plan.request.v1";
inline constexpr const char* ORDER_PLAN = "execution.plan.v1";
inline constexpr const char* NOTIONAL_ORDER_PLANNING_REQUEST = "execution.plan.notional.request.v1";
inline constexpr const char* NOTIONAL_ORDER_PLAN = "execution.plan.notional.v1";

// Private southbound gateway subjects used by the replay/test exchange backend.
inline constexpr const char* BACKEND_SUBMIT_ORDER = "gateway.backend.command.submit.v1";
inline constexpr const char* BACKEND_CANCEL_ORDER = "gateway.backend.command.cancel.v1";
inline constexpr const char* BACKEND_EXCHANGE_SNAPSHOT_REQUEST =
    "gateway.backend.snapshot.request.v1";
inline constexpr const char* BACKEND_ORDER_UPDATE = "gateway.backend.event.order_update.v1";
inline constexpr const char* BACKEND_FILL = "gateway.backend.event.fill.v1";
inline constexpr const char* BACKEND_EXCHANGE_SNAPSHOT = "gateway.backend.snapshot.v1";

inline std::vector<std::string> tradingRuntimeSubjects()
{
    return {
        MARKET_DATA_RELEASE,
        MARKET_DATA_UPDATED,
        MARKET_SLICE_CLOSED,
        MARKET_SLICE_SNAPSHOT,
        STRATEGY_INTENTS,
        DECISION_BATCH,
        EXECUTION_PRICES,
        SUBMIT_ORDER,
        CANCEL_ORDER,
        ORDER_UPDATE,
        FILL,
        ACCOUNT_SNAPSHOT,
        EXECUTION_CYCLE_COMPLETE,
        EXCHANGE_SNAPSHOT,
        ORDER_PLANNING_REQUEST,
        ORDER_PLAN,
        NOTIONAL_ORDER_PLANNING_REQUEST,
        NOTIONAL_ORDER_PLAN
    };
}

// Compatibility alias for older non-clock services. Runtime subjects are now purely
// trading subjects; temporal authority is local TimeHandler configuration.
inline std::vector<std::string> runtimeSubjects()
{
    return tradingRuntimeSubjects();
}

inline std::vector<std::string> exchangeGatewayControlSubjects()
{
    return {
        EXCHANGE_SNAPSHOT_REQUEST
    };
}

inline std::vector<std::string> exchangeBackendSubjects()
{
    return {
        BACKEND_SUBMIT_ORDER,
        BACKEND_CANCEL_ORDER,
        BACKEND_EXCHANGE_SNAPSHOT_REQUEST,
        BACKEND_ORDER_UPDATE,
        BACKEND_FILL,
        BACKEND_EXCHANGE_SNAPSHOT
    };
}
}
